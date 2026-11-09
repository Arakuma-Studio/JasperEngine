// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimationPanel.h"

#include "TitleAnimChecks.h"
#include "TitleAnimCost.h"
#include "TitleAnimOps.h"
#include "TitleAnimPresets.h"
#include "TitleAnimRender.h"
#include "TitleAnimTimelineOps.h"

#include "Backend.h"
#include "titleanim/TitleAnimDataIO.h"
#include "external/IconsFontAwesome6.h"
#include "action/Ease.h"
#include "animation/AnimationClipIO.h"
#include "audio/TimelineAudioPreview.h"
#include "math/Curve.h"
#include "render/TextureRender.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitleDesignerPrefs.h"
#include "titleart/designer/TitleDesignerState.h"
#include "widget/CurveEditor.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;

namespace {

    // a played or scrubbed pose stays at most this side (a synchronous render inside a frame)
    constexpr float MAX_POSE_SIDE = 960.0f;
    constexpr double POSE_SLICE_MS = 6.0;
    constexpr float MAX_PLAYING_POSE_SIDE = 640.0f;
    // the cached pose textures (2 MiB each, about, at the capped side)
    constexpr size_t POSE_CACHE = 48;
    constexpr float ROW = 20.0f;
    constexpr float RULER = 18.0f;
    constexpr float MARKERS = 18.0f;
    constexpr float TRACK_COLUMN = 300.0f;

    const ImU32 KEY_COLOR = IM_COL32(220, 220, 225, 255);
    const ImU32 KEY_SELECTED = IM_COL32(255, 200, 80, 255);
    const ImU32 GHOST_COLOR = IM_COL32(220, 220, 225, 70);
    const ImU32 PLAYHEAD_COLOR = IM_COL32(230, 80, 80, 255);

    struct PoseTexture {
        Texture gpu;
        ImTextureID id{};
        tt::TitleRectI frame;
        float pxScale = 1.0f;
        ~PoseTexture(){ gpu.destroy(); }
    };

    struct Panel {
        // the document's values, re-read when it changes
        uint64_t change = ~0ull;
        const void* document = nullptr;
        ta::TitleAnimationSet set;
        std::vector<std::string> trackLabels;
        std::unique_ptr<TitleAnimRenderer> renderer;

        double renderUs = -1.0;     // the document's static render estimate (-1: not planned yet)
        std::string animation;
        double time = 0.0, duration = 1.0, viewStart = 0.0, secondsPerPixel = 0.01;
        bool playing = false, previewing = false, loopPreview = true, frames = false, curves = false, record = false, onion = false,
             muted = false;
        std::string previewAs = "exact";
        int previewMode = 0;
        std::vector<TitleKeyRef> keySelection;
        std::vector<TitleKeyClipboard> clipboard;
        std::string status, lastRecordName;
        ui::CurveEditor curveEditor;
        std::vector<Curve> curves_;
        std::vector<TitleKeyRef> curveKeys;

        enum class Drag : uint8_t { None, Scrub, Keys, BoxScale, Box, End, LoopStart };
        Drag drag = Drag::None;
        ImVec2 dragStart;
        double dragTimeStart = 0.0, boxPivot = 0.0, boxSide = 0.0;
        // the animation as the drag found it: the document shows the staged batch of the last
        // frame, and a stage replaces, so each frame's ops are computed from the drag's start
        ta::TitleAnimation dragFrom;
        bool boxMaxSide = false;
        bool focused = false;
        std::string hoveredPreset;
        // where the timeline is on screen this frame (scripted input finds keys with it)
        struct { float x, rulerY, rulerH, rowsY, rowH; } geometry{0, 0, 0, 0, 0};

        // poses: the most recent at the back
        std::list<std::pair<uint64_t, std::unique_ptr<PoseTexture>>> poses;
        TitleDesignerState* state = nullptr;
        TitleDesignerSettings* settings = nullptr;

        // what the marker lane played this pass
        double soundLast = 0.0;
        TimelineAudioPreview audio;
    };

    Panel& panel(){
        static Panel p;
        return p;
    }

    uint64_t poseKey(const Panel& p, double time, const char* as, float scale){
        uint64_t h = p.change * 1000003ull;
        h ^= std::hash<std::string>()(p.animation) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= (uint64_t)std::llround(time * 1000.0) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>()(as) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        h ^= (uint64_t)std::llround(scale * 1000.0) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }

    const ta::TitleAnimation* current(const Panel& p){
        const int i = findTitleAnimation(p.set, p.animation);
        return i >= 0 ? &p.set.animations[(size_t)i] : nullptr;
    }

    double lengthOf(const ta::TitleAnimation& a){
        if (a.duration >= 0.0f) return a.duration;
        double end = 1.0;
        for (const ta::TitleTrack& t : a.tracks) for (const ta::TitleKey& k : t.keys) end = std::max(end, (double)k.key.time);
        for (const ta::TitleMarker& m : a.markers) end = std::max(end, (double)m.time);
        return end;
    }

    int fpsOf(const Panel& p, const ta::TitleAnimation& a){
        (void)a;
        return std::max(1, p.set.fps);
    }

    void refresh(Panel& p, TitleDesignerState& state){
        p.state = &state;
        if (p.document != state.document().get() || p.change != state.changeCount()){
            const bool external = p.document == state.document().get() && p.change != ~0ull && !state.gestureActive();
            if (external && p.playing) p.playing = false;       // R48: undo while playing pauses it
            if (p.document != state.document().get()){
                p.animation.clear();
                p.time = 0.0;
                p.previewing = false;
                p.playing = false;
                p.record = false;       // a new document never starts by keying its edits
                p.keySelection.clear();
            }
            p.document = state.document().get();
            p.change = state.changeCount();
            p.set = tt::readAnimationSet(state.shown());
            p.renderer.reset();
            p.renderUs = -1.0;
            p.poses.clear();
            if (findTitleAnimation(p.set, p.animation) < 0){
                p.animation = p.set.defaultName.empty() ? (p.set.animations.empty() ? std::string() : p.set.animations.front().name) : p.set.defaultName;
                p.keySelection.clear();
            }
        }
        const ta::TitleAnimation* a = current(p);
        p.duration = a ? lengthOf(*a) : 1.0;
        p.trackLabels.clear();
        if (a){
            for (const ta::TitleTrack& t : a->tracks){
                const tt::TrackFieldInfo info = tt::resolveTrackField(state.shown(), t.layer, t.field);
                const tt::Json* layer = t.layer.empty() ? nullptr : tt::findLayer(state.shown().root, t.layer);
                std::string owner = layer ? layer->value("name", t.layer) : std::string("Title");
                p.trackLabels.push_back(owner + " / " + (info.ok && !info.label.empty() ? info.label : t.field) + (t.orphan ? " (missing)" : ""));
            }
        }
    }

    TitleAnimRenderer* rendererOf(Panel& p, TitleDesignerState& state){
        if (!p.renderer) p.renderer = std::make_unique<TitleAnimRenderer>(state.project(), state.shownSnapshot());
        return p.renderer.get();
    }

    tt::Json opBase(const char* name, const std::string& animation){
        tt::Json op;
        op["op"] = name;
        if (!animation.empty()) op["animation"] = animation;
        return op;
    }

    bool commit(Panel& p, TitleDesignerState& state, const tt::Json& ops, const std::string& name){
        if (ops.empty()) return true;
        std::string error;
        if (!state.commit(ops, name, &error)){
            p.status = error;
            return false;
        }
        p.status.clear();
        return true;
    }

    // ---- gestures ----

    void cancelDrag(Panel& p, TitleDesignerState& state){
        if (state.gestureActive()) state.cancelGesture();
        p.drag = Panel::Drag::None;
    }

    void applyDragOps(Panel& p, TitleDesignerState& state, const tt::Json& ops){
        std::string error;
        if (!state.stage(ops, &error)) p.status = error;
    }

    // keys of the selection in the current document after a move (indices can change)
    void reselectAt(Panel& p, const ta::TitleAnimation& before, double shift, double pivot, double factor, int fps){
        std::vector<std::pair<std::pair<std::string, std::string>, double>> moved;
        for (const TitleKeyRef& r : p.keySelection){
            const ta::TitleTrack& t = before.tracks[(size_t)r.track];
            double time = t.keys[(size_t)r.key].key.time;
            time = pivot + (time - pivot) * factor + shift;
            moved.push_back({{t.layer, t.field}, snapTitleKeyTime(time, fps)});
        }
        p.keySelection.clear();
        const ta::TitleAnimation* a = current(p);
        if (!a) return;
        for (int track = 0; track < (int)a->tracks.size(); track++){
            const ta::TitleTrack& t = a->tracks[(size_t)track];
            for (const auto& m : moved){
                if (m.first.first != t.layer || m.first.second != t.field) continue;
                for (int key = 0; key < (int)t.keys.size(); key++)
                    if (std::fabs(t.keys[(size_t)key].key.time - m.second) <= 1e-3) p.keySelection.push_back({track, key});
            }
        }
    }

    // ---- the pose on the canvas ----

    bool poseActive(const Panel& p){
        return p.previewing && !p.animation.empty();
    }

    PoseTexture* poseAt(Panel& p, TitleDesignerState& state, double time, float scale, bool render){
        const uint64_t key = poseKey(p, time, p.previewAs.c_str(), scale);
        for (auto it = p.poses.begin(); it != p.poses.end(); ++it){
            if (it->first == key){
                p.poses.splice(p.poses.end(), p.poses, it);
                return p.poses.back().second.get();
            }
        }
        if (!render) return nullptr;
        TitleAnimRenderer* renderer = rendererOf(p, state);
        TitleAnimFrameRequest request;
        request.animation = p.animation;
        request.time = time;
        request.pxScale = scale;
        if (!titleAnimRenderAsFromName(p.previewAs, request.as)) request.as = TitleAnimRenderAs::Exact;
        TitleAnimFrame frame;
        std::string error;
        // R39: a new re-render steps here within the frame budget, the previous image staying;
        // title-live, layer-live and glyph values reuse the cached base and compose this frame
        if (!renderer->advancePose(request, POSE_SLICE_MS, error)){
            if (!error.empty()) p.status = error;
            Backend::getApp().requestRedraw();
            return nullptr;
        }
        if (!renderer->render(request, frame, error)){
            p.status = error;
            return nullptr;
        }
        if (frame.image.width <= 0 || frame.image.height <= 0) return nullptr;
        const size_t bytes = (size_t)frame.image.width * frame.image.height * 4;
        void* pixels = std::malloc(bytes);
        if (!pixels) return nullptr;
        std::memcpy(pixels, frame.image.data.data(), bytes);
        static uint64_t counter = 0;
        auto entry = std::make_unique<PoseTexture>();
        TextureData data(frame.image.width, frame.image.height, (unsigned int)bytes, ColorFormat::RGBA, 4, pixels);
        entry->gpu.setData("editor:title-animation-pose:" + std::to_string(++counter), data);
        entry->gpu.setMinFilter(TextureFilter::LINEAR);
        entry->gpu.setMagFilter(TextureFilter::LINEAR);
        TextureRender* gpu = entry->gpu.getRender();
        entry->gpu.releaseData();
        if (!gpu || !gpu->isCreated()) return nullptr;
        entry->id = Backend::getImGuiTexture(gpu);
        entry->frame = frame.frame;
        entry->pxScale = scale;
        p.poses.emplace_back(key, std::move(entry));
        while (p.poses.size() > POSE_CACHE) p.poses.pop_front();
        return p.poses.back().second.get();
    }

    void drawPoseImage(ImDrawList* dl, const PoseTexture& pose, float docW, float docH, const CanvasXform& xform, ImU32 tint){
        // frame in canvas pixels, y down from the canvas' top left; the canvas is centred on the origin
        const float x0 = (float)pose.frame.x - docW * 0.5f, y0 = docH * 0.5f - (float)pose.frame.y;
        const ImVec2 a = xform.toScreen({x0, y0});
        const ImVec2 b = xform.toScreen({x0 + (float)pose.frame.width, y0 - (float)pose.frame.height});
        dl->AddImage(pose.id, a, b, ImVec2(0, 0), ImVec2(1, 1), tint);
    }

    float poseScaleFor(const tt::TitleArtDoc& doc, float zoom, bool playing){
        const float side = (float)std::max(doc.canvasWidth(), doc.canvasHeight());
        const float wanted = std::max(zoom, 0.05f);
        return std::min(wanted, (playing ? MAX_PLAYING_POSE_SIDE : MAX_POSE_SIDE) / std::max(side, 1.0f));
    }

    void drawCanvasPose(TitleDesignerState& state, ImDrawList* dl, ImVec2 min, ImVec2 max, const CanvasXform& xform){
        Panel& p = panel();
        refresh(p, state);
        const tt::TitleArtDoc& doc = state.shown();
        const float scale = poseScaleFor(doc, xform.zoom, p.playing);
        PoseTexture* fresh = poseAt(p, state, p.time, scale, true);
        PoseTexture* pose = fresh ? fresh : p.poses.empty() ? nullptr : p.poses.back().second.get();
        dl->PushClipRect(min, max, true);
        const float w = (float)doc.canvasWidth(), h = (float)doc.canvasHeight();
        if (pose) drawPoseImage(dl, *pose, w, h, xform, IM_COL32_WHITE);
        if (p.onion && pose && !p.playing){
            // R47: ghosts only from poses already cached
            const ta::TitleAnimation* a = current(p);
            const double frame = 1.0 / fpsOf(p, a ? *a : ta::TitleAnimation());
            const int before = p.settings ? p.settings->onionBefore : 2, after = p.settings ? p.settings->onionAfter : 2;
            const int alpha = (int)std::lround(255.0f * (p.settings ? p.settings->onionOpacity : 0.25f));
            for (int k = -before; k <= after; k++){
                if (k == 0) continue;
                const uint64_t key = poseKey(p, p.time + k * frame, p.previewAs.c_str(), scale);
                for (const auto& entry : p.poses)
                    if (entry.first == key) drawPoseImage(dl, *entry.second, w, h, xform, k < 0 ? IM_COL32(255, 150, 150, alpha) : IM_COL32(150, 255, 150, alpha));
            }
        }
        dl->PopClipRect();
    }

    // ---- the track list and the timeline ----

    float xOf(const Panel& p, double t, float left){
        return left + (float)((t - p.viewStart) / p.secondsPerPixel);
    }
    double timeAt(const Panel& p, float x, float left){
        return p.viewStart + (x - left) * p.secondsPerPixel;
    }

    std::string formatValue(const tt::Json& v){
        char buf[64];
        if (v.is_number()){
            std::snprintf(buf, sizeof(buf), "%.2f", v.get<double>());
            return buf;
        }
        if (v.is_array()){
            std::string out;
            for (const tt::Json& e : v){
                if (!out.empty()) out += " ";
                if (e.is_number()){
                    std::snprintf(buf, sizeof(buf), "%.1f", e.get<double>());
                    out += buf;
                }
            }
            return out;
        }
        if (v.is_string()) return v.get<std::string>();
        if (v.is_boolean()) return v.get<bool>() ? "On" : "Off";
        return "";
    }

    // the field's class icon for the mode, its tooltip's reason and colour (R44)
    void classIcon(Panel& p, TitleDesignerState& state, const ta::TitleTrack& t, int trackIndex, const char*& icon, ImU32& color, std::string& why){
        const tt::TrackFieldInfo info = tt::resolveTrackField(state.shown(), t.layer, t.field);
        const TitleArtRenderMode mode = p.previewAs == "baked" ? TitleArtRenderMode::BAKED : p.previewAs == "glyph_atlas" ? TitleArtRenderMode::GLYPH_ATLAS : TitleArtRenderMode::REALTIME;
        icon = ICON_FA_ROTATE;
        color = IM_COL32(230, 170, 40, 255);
        why = (info.ok && !info.label.empty() ? info.label : t.field) + " re-renders the layer.";
        TitleAnimRenderer* renderer = rendererOf(p, state);
        const tt::TitleAnimPlan& plan = renderer->plan(mode);
        const tt::AnimationPlan* ap = plan.find(p.animation);
        if (!ap || trackIndex >= (int)ap->tracks.size()) return;
        const tt::TrackPlan& tp = ap->tracks[(size_t)trackIndex];
        if (tp.play == tt::TrackPlay::NotPlayed){
            icon = ICON_FA_BAN;
            color = IM_COL32(230, 80, 80, 255);
            why = "Not played in " + p.previewAs + ": " + tp.reason;
            return;
        }
        if (tp.how == "companion" || tp.how == "glyph_cells" || tp.how == "cells_swap"){
            icon = tp.how == "glyph_cells" || tp.how == "cells_swap" ? ICON_FA_FONT : ICON_FA_BOLT;
            color = IM_COL32(80, 200, 110, 255);
            why = tp.how == "companion" ? "Live: drawn each frame on the title's quad, no re-render." : "Live: glyph cells compose each frame.";
        }else if (tp.how == "flipbook"){
            icon = ICON_FA_ROTATE;
            color = IM_COL32(230, 170, 40, 255);
            why += " Baked: flipbook frames.";
        }else{
            why += " Realtime: re-renders, at most rerender_fps a second.";
        }
        if (!tp.reason.empty()) why += " " + tp.reason;
    }

    void addKeyAt(Panel& p, TitleDesignerState& state, const ta::TitleTrack& t){
        tt::Json op = opBase("set_key", p.animation);
        op["layer"] = titleLayerRef(t.layer);
        op["field"] = t.field;
        op["time"] = p.time;
        tt::Json ops = tt::Json::array();
        ops.push_back(op);
        commit(p, state, ops, "Add Key");
    }

    void drawKeyMenu(Panel& p, TitleDesignerState& state, const ta::TitleAnimation& a){
        if (!ImGui::BeginPopup("##keymenu")) return;
        if (ImGui::BeginMenu("Ease")){
            for (int i = 0; i <= (int)EaseType::STEP; i++){
                const EaseType e = (EaseType)i;
                if (e == EaseType::CUSTOM || e == EaseType::CURVE) continue;
                if (ImGui::MenuItem(anim::easeName(e))) commit(p, state, titleSetEaseOps(a, p.keySelection, e), "Ease Keys");
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tangents")){
            static const std::pair<const char*, ClipTangentMode> modes[] = {
                {"Clamped Auto", ClipTangentMode::ClampedAuto}, {"Auto", ClipTangentMode::Auto}, {"Free", ClipTangentMode::Free},
                {"Flat", ClipTangentMode::Flat}, {"Broken", ClipTangentMode::Broken}, {"Linear", ClipTangentMode::Linear}, {"Constant", ClipTangentMode::Constant}};
            for (const auto& m : modes) if (ImGui::MenuItem(m.first)) commit(p, state, titleSetTangentModeOps(a, p.keySelection, m.second), "Set Tangents");
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Delete")) commit(p, state, titleDeleteKeysOps(a, p.keySelection), "Delete Keys");
        ImGui::EndPopup();
    }

    void drawTrackMenu(Panel& p, TitleDesignerState& state, const ta::TitleAnimation& a, int track){
        if (!ImGui::BeginPopup("##trackmenu")) return;
        const ta::TitleTrack& t = a.tracks[(size_t)track];
        auto trackOp = [&](const char* name){
            tt::Json op = opBase(name, a.name);
            op["layer"] = titleLayerRef(t.layer);
            op["field"] = t.field;
            return op;
        };
        if (ImGui::MenuItem("Relative", nullptr, t.relative)){
            tt::Json op = trackOp("set_track");
            op["relative"] = !t.relative;
            commit(p, state, tt::Json::array({op}), "Set Relative");
        }
        if (ImGui::BeginMenu("Stagger")){
            for (const char* order : {"forward", "reverse", "center_out", "edges_in", "random"}){
                if (ImGui::MenuItem(order)){
                    tt::Json op = trackOp("set_track");
                    tt::Json stagger;
                    stagger["each"] = 0.05;
                    stagger["order"] = order;
                    op["stagger"] = stagger;
                    commit(p, state, tt::Json::array({op}), "Set Stagger");
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Pivot")){
            for (const char* pivot : {"origin", "center", "bottom", "top", "left", "right", "baseline"}){
                if (ImGui::MenuItem(pivot)){
                    tt::Json op = trackOp("set_track");
                    op["pivot"] = pivot;
                    commit(p, state, tt::Json::array({op}), "Set Pivot");
                }
            }
            ImGui::EndMenu();
        }
        if (!t.drive && ImGui::MenuItem("Convert to Drive")){
            const tt::TrackFieldInfo info = tt::resolveTrackField(state.shown(), t.layer, t.field);
            if (t.keys.size() >= 2 && !info.stepped){
                tt::Json op = trackOp("set_track");
                tt::Json drive;
                drive["from"] = t.keys.front().key.value;
                drive["to"] = t.keys.back().key.value;
                drive["cycles"] = 1.0;
                op["drive"] = drive;
                commit(p, state, tt::Json::array({op}), "Convert to Drive");
            }
        }
        if (ImGui::MenuItem("Remove")) commit(p, state, tt::Json::array({trackOp("remove_track")}), "Remove Track");
        ImGui::EndPopup();
    }

    bool hasEases(const ta::TitleTrack& t){
        for (const ta::TitleKey& k : t.keys) if (k.key.ease != EaseType::LINEAR && k.key.ease != EaseType::STEP) return true;
        return false;
    }

    // ---- curves (R43): one curve per component of the selected tracks; edits replace the keys ----

    void drawCurves(Panel& p, TitleDesignerState& state, const ta::TitleAnimation& a, ImVec2 size){
        std::set<int> tracks;
        for (const TitleKeyRef& r : p.keySelection) tracks.insert(r.track);
        if (tracks.empty()) for (int i = 0; i < (int)a.tracks.size() && i < 8; i++) tracks.insert(i);
        std::vector<Curve> curves;
        std::vector<ui::CurveBinding> bindings;
        std::vector<std::pair<int, int>> owners;
        std::vector<std::string> labels;
        for (int track : tracks){
            const ta::TitleTrack& t = a.tracks[(size_t)track];
            if (t.drive || t.reseed || t.keys.empty() || !t.keys.front().stepped.empty() || t.keys.front().key.value.empty()) continue;
            if (hasEases(t)){
                ImGui::TextDisabled("%s has eased keys", p.trackLabels[(size_t)track].c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton((std::string("Convert Eases to Curves##") + std::to_string(track)).c_str()))
                    commit(p, state, titleConvertEasesOps(a, track), "Convert Eases to Curves");
                continue;
            }
            const size_t width = t.keys.front().key.value.size();
            for (size_t c = 0; c < width; c++){
                Curve curve;
                for (size_t i = 0; i < t.keys.size(); i++){
                    const ta::TitleKey& k = t.keys[i];
                    CurveKey key;
                    key.time = k.key.time;
                    key.value = k.key.value[c];
                    if (k.key.hasTangents()){
                        key.inTangent = k.key.inTangent[c];
                        key.outTangent = k.key.outTangent[c];
                    }else{
                        // a linear key's segments are straight: a flat Hermite tangent would draw
                        // (and, once edited, play) an S curve
                        auto slope = [&](size_t a, size_t b){
                            const float dt = t.keys[b].key.time - t.keys[a].key.time;
                            return dt > 1e-6f ? (t.keys[b].key.value[c] - t.keys[a].key.value[c]) / dt : 0.0f;
                        };
                        if (i > 0) key.inTangent = slope(i - 1, i);
                        if (i + 1 < t.keys.size()) key.outTangent = slope(i, i + 1);
                        if (i == 0) key.inTangent = key.outTangent;
                        if (i + 1 == t.keys.size()) key.outTangent = key.inTangent;
                    }
                    curve.keys.push_back(key);
                }
                curves.push_back(curve);
                owners.push_back({track, (int)c});
                labels.push_back(p.trackLabels[(size_t)track] + (width > 1 ? std::string(".") + "xyzw"[c] : std::string()));
            }
        }
        if (curves.empty()){
            ImGui::TextDisabled("Select tracks with numeric keys to see their curves.");
            return;
        }
        static const ImU32 colors[] = {IM_COL32(230, 90, 90, 255), IM_COL32(100, 200, 110, 255), IM_COL32(100, 150, 240, 255), IM_COL32(230, 200, 90, 255)};
        for (size_t i = 0; i < curves.size(); i++){
            ui::CurveBinding b;
            b.curve = &curves[i];
            b.color = colors[i % 4];
            b.label = labels[i].c_str();
            bindings.push_back(b);
        }
        const ui::CurveEditorResult result = p.curveEditor.draw("##titlecurves", bindings, size);
        if (result.began) state.beginGesture("Edit Curves");
        if (result.changed){
            tt::Json ops = tt::Json::array();
            for (int track : tracks){
                const ta::TitleTrack& t = a.tracks[(size_t)track];
                std::vector<int> comps;
                for (size_t i = 0; i < owners.size(); i++) if (owners[i].first == track) comps.push_back((int)i);
                if (comps.empty()) continue;
                std::vector<ta::TitleKey> keys;
                const Curve& first = curves[(size_t)comps[0]];
                for (size_t k = 0; k < first.keys.size(); k++){
                    ta::TitleKey key = k < t.keys.size() ? t.keys[k] : t.keys.back();
                    key.key.time = first.keys[k].time;
                    key.key.ease = EaseType::LINEAR;
                    // the key menu's choice (R43): the op recomputes the mode's tangents
                    const CurveKey& modeOf = first.keys[k];
                    if (modeOf.broken()) key.key.tangentMode = ClipTangentMode::Broken;
                    else if (modeOf.outMode() == CurveTangentMode::Auto) key.key.tangentMode = ClipTangentMode::Auto;
                    else if (modeOf.outMode() == CurveTangentMode::ClampedAuto) key.key.tangentMode = ClipTangentMode::ClampedAuto;
                    else if (modeOf.outMode() == CurveTangentMode::Linear) key.key.tangentMode = ClipTangentMode::Linear;
                    else if (modeOf.outMode() == CurveTangentMode::Constant) key.key.tangentMode = ClipTangentMode::Constant;
                    else key.key.tangentMode = ClipTangentMode::Free;
                    key.key.value.resize(comps.size());
                    key.key.inTangent.resize(comps.size());
                    key.key.outTangent.resize(comps.size());
                    for (size_t c = 0; c < comps.size(); c++){
                        const CurveKey& ck = curves[(size_t)comps[c]].keys[k];
                        key.key.value[c] = ck.value;
                        key.key.inTangent[c] = ck.inTangent;
                        key.key.outTangent[c] = ck.outTangent;
                    }
                    keys.push_back(key);
                }
                tt::Json op = opBase("set_track", a.name);
                op["layer"] = titleLayerRef(t.layer);
                op["field"] = t.field;
                tt::Json array = tt::Json::array();
                for (const ta::TitleKey& k : keys) array.push_back(ta::encodeKey(k));
                op["keys"] = array;
                ops.push_back(op);
            }
            if (!state.gestureActive()) state.beginGesture("Edit Curves");
            applyDragOps(p, state, ops);
        }
        if (result.ended && state.gestureActive()){
            std::string error;
            if (!state.endGesture(&error)) p.status = error;
        }
        if (result.cancelled && state.gestureActive()) state.cancelGesture();
    }

    // ---- the dopesheet (R42) ----

    void drawDopesheet(Panel& p, TitleDesignerState& state, const ta::TitleAnimation& a, ImVec2 size){
        const int fps = fpsOf(p, a);
        const float scale = ImGui::GetFontSize() / 15.0f;
        const float rowH = ROW * scale, rulerH = RULER * scale, markersH = MARKERS * scale;
        const float columnW = TRACK_COLUMN * scale;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float timelineX = origin.x + columnW, timelineW = std::max(40.0f, size.x - columnW);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::Dummy(ImVec2(columnW, rulerH + markersH));
        const float rowsTop = origin.y + rulerH + markersH;
        // ruler and markers lane
        dl->AddRectFilled(ImVec2(timelineX, origin.y), ImVec2(timelineX + timelineW, rowsTop), IM_COL32(45, 45, 50, 255));
        const ImVec2 rulerMin(timelineX, origin.y + markersH), rulerMax(timelineX + timelineW, rowsTop);
        // the ticks, every frame group that's at least 40 px apart
        const double visibleSpan = timelineW * p.secondsPerPixel;
        double tick = 1.0 / fps;
        while (tick / p.secondsPerPixel < 40.0f * scale) tick *= tick * fps < 2.0 ? 5.0 : 2.0;
        char buf[48];
        for (double t = std::floor(p.viewStart / tick) * tick; t <= p.viewStart + visibleSpan; t += tick){
            const float x = xOf(p, t, timelineX);
            dl->AddLine(ImVec2(x, rulerMax.y - 6.0f * scale), ImVec2(x, rulerMax.y), IM_COL32(150, 150, 150, 255));
            if (p.frames) std::snprintf(buf, sizeof(buf), "%d", (int)std::round(t * fps));
            else std::snprintf(buf, sizeof(buf), "%.2f", t);
            dl->AddText(ImVec2(x + 2.0f, rulerMin.y), IM_COL32(170, 170, 175, 255), buf);
        }
        // the end handle and the loop section
        const float endX = xOf(p, p.duration, timelineX);
        dl->AddRectFilled(ImVec2(timelineX, rulerMin.y), ImVec2(endX, rulerMin.y + 3.0f), IM_COL32(90, 150, 240, 200));
        if (a.wrap != ta::AnimWrap::Once){
            const float loopX = xOf(p, a.loopStart, timelineX);
            dl->AddRectFilled(ImVec2(loopX, rulerMax.y - 3.0f), ImVec2(endX, rulerMax.y), IM_COL32(100, 200, 110, 220));
        }
        // markers
        for (const ta::TitleMarker& m : a.markers){
            const float x = xOf(p, m.time, timelineX);
            const ImU32 c = m.sound.empty() ? IM_COL32(230, 180, 60, 255) : IM_COL32(120, 200, 240, 255);
            dl->AddTriangleFilled(ImVec2(x, origin.y + 2.0f), ImVec2(x + 8.0f * scale, origin.y + 6.0f * scale), ImVec2(x, origin.y + 11.0f * scale), c);
            dl->AddLine(ImVec2(x, origin.y + 2.0f), ImVec2(x, origin.y + markersH), c);
            std::string text = m.name;
            if (!m.sound.empty()) text = std::string(ICON_FA_VOLUME_HIGH " ") + text;
            dl->AddText(ImVec2(x + 10.0f * scale, origin.y + 1.0f), IM_COL32(220, 220, 220, 255), text.c_str());
        }
        // input areas
        ImGui::SetCursorScreenPos(ImVec2(timelineX, origin.y));
        ImGui::InvisibleButton("##rulerstrip", ImVec2(timelineW, rulerH + markersH));
        const bool rulerHovered = ImGui::IsItemHovered();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (rulerHovered && ImGui::IsMouseClicked(0)){
            p.previewing = true;
            p.playing = false;
            p.drag = std::fabs(mouse.x - endX) < 6.0f * scale ? Panel::Drag::End : Panel::Drag::Scrub;
            p.dragStart = mouse;
        }
        if (rulerHovered && std::fabs(mouse.x - endX) < 6.0f * scale) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (rulerHovered && ImGui::GetIO().MouseWheel != 0.0f && ImGui::GetIO().KeyCtrl){
            const double at = timeAt(p, mouse.x, timelineX);
            p.secondsPerPixel = std::clamp(p.secondsPerPixel * std::pow(0.9, ImGui::GetIO().MouseWheel), 1e-4, 1.0);
            p.viewStart = std::max(0.0, at - (mouse.x - timelineX) * p.secondsPerPixel);
        }
        // the rows
        ImGui::SetCursorScreenPos(ImVec2(origin.x, rowsTop));
        ImGui::BeginChild("##titleanimrows", ImVec2(size.x, std::max(rowH, size.y - rulerH - markersH)), false, ImGuiWindowFlags_NoScrollbar);
        ImDrawList* rows = ImGui::GetWindowDrawList();
        const ImVec2 rowsOrigin = ImGui::GetCursorScreenPos();
        // the timeline's own item: over the track column it would take the row buttons' hover
        ImGui::SetCursorScreenPos(ImVec2(timelineX, rowsOrigin.y));
        ImGui::InvisibleButton("##timelinehit", ImVec2(timelineW, std::max(rowH * (float)a.tracks.size(), rowH)));
        const bool timelineHovered = ImGui::IsItemHovered();
        p.geometry = {timelineX, origin.y + markersH, rulerH, rowsOrigin.y, rowH};
        int contextTrack = -1;
        // key hits, for picking and the track menus
        std::vector<std::pair<ImVec2, TitleKeyRef>> keyPoints;
        for (int track = 0; track < (int)a.tracks.size(); track++){
            const ta::TitleTrack& t = a.tracks[(size_t)track];
            const float y = rowsOrigin.y + rowH * (float)track;
            rows->AddRectFilled(ImVec2(timelineX, y), ImVec2(timelineX + timelineW, y + rowH), track % 2 ? IM_COL32(38, 38, 42, 255) : IM_COL32(42, 42, 46, 255));
            const char* icon;
            ImU32 color;
            std::string why;
            classIcon(p, state, t, track, icon, color, why);
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 2.0f, y));
            ImGui::PushID(track);
            if (ImGui::SmallButton(t.mute ? ICON_FA_VOLUME_XMARK : ICON_FA_VOLUME_HIGH)){
                tt::Json op = opBase("set_track", a.name);
                op["layer"] = titleLayerRef(t.layer);
                op["field"] = t.field;
                op["mute"] = !t.mute;
                commit(p, state, tt::Json::array({op}), "Mute Track");
            }
            ImGui::SameLine();
            ImGui::TextColored(ImColor(color), "%s", icon);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", why.c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(p.trackLabels[(size_t)track].c_str());
            if (ImGui::IsItemClicked(1)){
                ImGui::OpenPopup("##trackmenu");
                contextTrack = track;
            }
            // the value at the playhead, right-aligned (R42)
            const tt::Json value = sampleTitleTrackValue(state.shown(), a, t.layer, t.field, p.time);
            const std::string text = formatValue(value);
            const float textX = timelineX - ImGui::CalcTextSize(text.c_str()).x - 8.0f * scale;
            rows->AddText(ImVec2(textX, y + 2.0f), t.drive ? IM_COL32(130, 200, 240, 255) : IM_COL32(190, 190, 195, 255), text.c_str());
            if (track == contextTrack || ImGui::IsPopupOpen("##trackmenu")) drawTrackMenu(p, state, a, track);
            ImGui::PopID();
            // keys
            for (int key = 0; key < (int)t.keys.size(); key++){
                const float kx = xOf(p, t.keys[(size_t)key].key.time, timelineX);
                const float cy = y + rowH * 0.5f, r = 5.0f * scale;
                const bool selected = std::find(p.keySelection.begin(), p.keySelection.end(), TitleKeyRef{track, key}) != p.keySelection.end();
                // the segment's ease as a curve glyph
                if (key + 1 < (int)t.keys.size() && t.keys[(size_t)key].key.ease != EaseType::LINEAR && !t.keys[(size_t)key].key.hasTangents()){
                    const float nx = xOf(p, t.keys[(size_t)key + 1].key.time, timelineX);
                    const std::vector<float> ease = titleEaseSamples(t.keys[(size_t)key].key.ease, 14);
                    ImVec2 last(kx, cy + rowH * 0.3f - ease[0] * rowH * 0.6f);
                    for (size_t s = 1; s < ease.size(); s++){
                        const ImVec2 q(kx + (nx - kx) * (float)s / 13.0f, cy + rowH * 0.3f - ease[s] * rowH * 0.6f);
                        rows->AddLine(last, q, IM_COL32(110, 160, 230, 180), 1.0f);
                        last = q;
                    }
                }
                ImVec2 quad[4] = {ImVec2(kx, cy - r), ImVec2(kx + r, cy), ImVec2(kx, cy + r), ImVec2(kx - r, cy)};
                rows->AddConvexPolyFilled(quad, 4, selected ? KEY_SELECTED : KEY_COLOR);
                keyPoints.push_back({ImVec2(kx, cy), {track, key}});
                // stagger's fan of ghost keys (R42): the first few units
                if (t.stagger){
                    for (int unit = 1; unit <= 3; unit++){
                        const float gx = xOf(p, t.keys[(size_t)key].key.time + unit * t.stagger->each, timelineX);
                        ImVec2 g[4] = {ImVec2(gx, cy - r * 0.7f), ImVec2(gx + r * 0.7f, cy), ImVec2(gx, cy + r * 0.7f), ImVec2(gx - r * 0.7f, cy)};
                        rows->AddConvexPolyFilled(g, 4, GHOST_COLOR);
                    }
                }
            }
        }
        // picking and key drags
        TitleKeyRef hit;
        double hitDist = 8.0 * scale;
        for (const auto& k : keyPoints){
            const double d = std::hypot(mouse.x - k.first.x, mouse.y - k.first.y);
            if (d < hitDist){
                hitDist = d;
                hit = k.second;
            }
        }
        const ImGuiIO& io = ImGui::GetIO();
        auto isSelected = [&](const TitleKeyRef& r){ return std::find(p.keySelection.begin(), p.keySelection.end(), r) != p.keySelection.end(); };
        if (timelineHovered && ImGui::IsMouseDoubleClicked(0) && hit.track < 0){
            // double-click adds a key on the row with the sampled value (R42)
            const int track = (int)((mouse.y - rowsOrigin.y) / rowH);
            if (track >= 0 && track < (int)a.tracks.size()){
                tt::Json op = opBase("set_key", a.name);
                op["layer"] = titleLayerRef(a.tracks[(size_t)track].layer);
                op["field"] = a.tracks[(size_t)track].field;
                op["time"] = snapTitleKeyTime(std::max(0.0, timeAt(p, mouse.x, timelineX)), fps);
                commit(p, state, tt::Json::array({op}), "Add Key");
            }
        }else if (timelineHovered && ImGui::IsMouseClicked(0)){
            if (hit.track >= 0){
                if (io.KeyCtrl){
                    auto it = std::find(p.keySelection.begin(), p.keySelection.end(), hit);
                    if (it != p.keySelection.end()) p.keySelection.erase(it);
                    else p.keySelection.push_back(hit);
                }else if (!isSelected(hit)){
                    p.keySelection = {hit};
                }
                p.drag = Panel::Drag::Keys;
                p.dragStart = mouse;
                p.dragFrom = a;
                p.dragTimeStart = a.tracks[(size_t)hit.track].keys[(size_t)hit.key].key.time;
            }else{
                // an empty press starts a box; a box edge on the selection scales
                if (!io.KeyCtrl) p.keySelection.clear();
                p.drag = Panel::Drag::Box;
                p.dragStart = mouse;
            }
        }
        if (timelineHovered && ImGui::IsMouseClicked(1)){
            if (hit.track >= 0){
                if (!isSelected(hit)) p.keySelection = {hit};
                ImGui::OpenPopup("##keymenu");
            }
        }
        drawKeyMenu(p, state, a);
        // the running drag
        if (p.drag == Panel::Drag::Scrub && ImGui::IsMouseDown(0)){
            p.time = std::max(0.0, snapTitleKeyTime(timeAt(p, mouse.x, timelineX), io.KeyAlt ? 0 : fps));
        }else if (p.drag == Panel::Drag::End && ImGui::IsMouseDown(0)){
            const double t = std::max(0.01, snapTitleKeyTime(timeAt(p, mouse.x, timelineX), fps));
            if (!state.gestureActive()) state.beginGesture("Set Duration");
            tt::Json op = opBase("set_animation", a.name);
            op["values"]["duration"] = t;
            applyDragOps(p, state, tt::Json::array({op}));
        }else if (p.drag == Panel::Drag::Keys && ImGui::IsMouseDown(0)){
            double shift = (mouse.x - p.dragStart.x) * p.secondsPerPixel;
            if (io.KeyShift) shift = 0.0;   // R42: lock the time
            const int snap = io.KeyAlt ? 0 : fps;
            if (std::fabs(mouse.x - p.dragStart.x) > 2.0f){
                if (!state.gestureActive()) state.beginGesture("Move Keys");
                applyDragOps(p, state, titleMoveKeysOps(p.dragFrom, p.keySelection, shift, snap));
            }
        }else if (p.drag == Panel::Drag::Box && ImGui::IsMouseDown(0)){
            const ImVec2 a0(std::min(p.dragStart.x, mouse.x), std::min(p.dragStart.y, mouse.y));
            const ImVec2 b0(std::max(p.dragStart.x, mouse.x), std::max(p.dragStart.y, mouse.y));
            rows->AddRectFilled(a0, b0, IM_COL32(90, 150, 240, 50));
            rows->AddRect(a0, b0, IM_COL32(90, 150, 240, 200));
        }else if (p.drag == Panel::Drag::BoxScale && ImGui::IsMouseDown(0)){
            const double t = timeAt(p, mouse.x, timelineX);
            const double factor = (t - p.boxPivot) / std::max(1e-4, p.boxSide - p.boxPivot);
            if (!state.gestureActive()) state.beginGesture("Scale Keys");
            applyDragOps(p, state, titleScaleKeysOps(p.dragFrom, p.keySelection, p.boxPivot, std::max(0.01, factor), io.KeyAlt ? 0 : fps));
        }
        if (p.drag != Panel::Drag::None && ImGui::IsMouseReleased(0)){
            if (p.drag == Panel::Drag::Box){
                const float x0 = std::min(p.dragStart.x, mouse.x), x1 = std::max(p.dragStart.x, mouse.x);
                const float y0 = std::min(p.dragStart.y, mouse.y), y1 = std::max(p.dragStart.y, mouse.y);
                for (const auto& k : keyPoints)
                    if (k.first.x >= x0 && k.first.x <= x1 && k.first.y >= y0 && k.first.y <= y1 && !isSelected(k.second)) p.keySelection.push_back(k.second);
            }
            if (state.gestureActive()){
                const ta::TitleAnimation before = p.dragFrom;
                std::string error;
                if (!state.endGesture(&error)) p.status = error;
                refresh(p, state);
                if (p.drag == Panel::Drag::Keys) reselectAt(p, before, io.KeyShift ? 0.0 : (mouse.x - p.dragStart.x) * p.secondsPerPixel, 0.0, 1.0, fps);
                else if (p.drag == Panel::Drag::BoxScale) reselectAt(p, before, 0.0, p.boxPivot, std::max(0.01, (timeAt(p, mouse.x, timelineX) - p.boxPivot) / std::max(1e-4, p.boxSide - p.boxPivot)), fps);
            }
            p.drag = Panel::Drag::None;
        }
        if (p.drag != Panel::Drag::None && ImGui::IsKeyPressed(ImGuiKey_Escape)) cancelDrag(p, state);
        // box edges of the selection: time-scale handles (R42)
        if (p.keySelection.size() > 1 && p.drag == Panel::Drag::None){
            double t0 = 1e30, t1 = -1e30;
            for (const TitleKeyRef& r : p.keySelection){
                const double t = a.tracks[(size_t)r.track].keys[(size_t)r.key].key.time;
                t0 = std::min(t0, t);
                t1 = std::max(t1, t);
            }
            const float bx0 = xOf(p, t0, timelineX), bx1 = xOf(p, t1, timelineX);
            rows->AddRect(ImVec2(bx0 - 4.0f, rowsOrigin.y), ImVec2(bx1 + 4.0f, rowsOrigin.y + rowH * (float)a.tracks.size()), IM_COL32(90, 150, 240, 140));
            if (timelineHovered && std::fabs(mouse.x - bx1) < 5.0f){
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsMouseClicked(0)){
                    p.drag = Panel::Drag::BoxScale;
                    p.boxPivot = t0;
                    p.boxSide = t1;
                    p.dragStart = mouse;
                    p.dragFrom = a;
                }
            }else if (timelineHovered && std::fabs(mouse.x - bx0) < 5.0f){
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsMouseClicked(0)){
                    p.drag = Panel::Drag::BoxScale;
                    p.boxPivot = t1;
                    p.boxSide = t0;
                    p.dragStart = mouse;
                    p.dragFrom = a;
                }
            }
        }
        ImGui::EndChild();
        // the playhead over everything
        const float headX = xOf(p, p.time, timelineX);
        dl->AddLine(ImVec2(headX, origin.y), ImVec2(headX, origin.y + size.y), PLAYHEAD_COLOR, 1.5f);
    }

    // ---- transport ----

    void stopPreview(Panel& p){
        p.playing = false;
        p.previewing = false;
        p.time = 0.0;
        p.audio.stopAll();
    }

    void advance(Panel& p, TitleDesignerState& state, const ta::TitleAnimation& a){
        if (!p.playing) return;
        const double dt = ImGui::GetIO().DeltaTime * std::max(0.01f, a.speed);
        double next = p.time + dt;
        if (next >= p.duration){
            if (p.loopPreview || a.wrap != ta::AnimWrap::Once) next = a.loopStart + std::fmod(next - p.duration, std::max(1e-3, p.duration - a.loopStart));
            else{
                next = p.duration;
                p.playing = false;
            }
        }
        // R46: sound markers play during preview, in an editor voice outside the mixer groups
        if (!p.muted){
            for (const ta::TitleMarker& m : a.markers){
                if (m.sound.empty()) continue;
                const bool crossed = next >= p.time ? (m.time > p.time && m.time <= next) : (m.time > p.time || m.time <= next);
                if (crossed) p.audio.play(state.project(), m.sound.path, m.volumeDb, 1.0f, 0, false);
            }
        }
        p.time = next;
    }

    void addPreset(Panel& p, TitleDesignerState& state, const TitleAnimPreset& preset){
        tt::Json op = opBase("apply_animation_preset", p.animation);
        op["preset"] = preset.id;
        commit(p, state, tt::Json::array({op}), "Add Preset " + preset.name);
    }

    void drawPresetMenu(Panel& p, TitleDesignerState& state){
        std::map<std::string, std::vector<const TitleAnimPreset*>> groups;
        for (const TitleAnimPreset& preset : TitleAnimPresets::all()) groups[preset.group].push_back(&preset);
        bool hovering = false;
        for (const auto& [group, presets] : groups){
            if (!ImGui::BeginMenu(group.c_str())) continue;
            for (const TitleAnimPreset* preset : presets){
                if (ImGui::MenuItem(preset->name.c_str())){
                    if (p.animation.empty()) continue;
                    addPreset(p, state, *preset);
                }
                if (ImGui::IsItemHovered()){
                    hovering = true;
                    if (!preset->description.empty()) ImGui::SetTooltip("%s", preset->description.c_str());
                    // a hover previews the preset's ops on the document without a step (R41), once
                    // per item: applying them every frame would re-render every frame
                    if (p.hoveredPreset == preset->id) continue;
                    p.hoveredPreset = preset->id;
                    PreviewItem item;
                    item.kind = PreviewItem::Kind::Effect;
                    item.id = preset->id;
                    item.label = preset->name;
                    item.stepName = "Add Preset " + preset->name;
                    tt::Json op = opBase("apply_animation_preset", p.animation);
                    op["preset"] = preset->id;
                    item.ops = tt::Json::array({op});
                    std::string error;
                    if (!p.animation.empty()) state.setPreview(item, &error);
                }
            }
            ImGui::EndMenu();
        }
        if (!hovering && !p.hoveredPreset.empty()){
            state.clearPreview();
            p.hoveredPreset.clear();
        }
    }

    void newAnimation(Panel& p, TitleDesignerState& state){
        tt::Json op = opBase("add_animation", std::string());
        op["name"] = uniqueTitleAnimationName(p.set, "New Animation");
        commit(p, state, tt::Json::array({op}), "New Animation");
        p.animation = op["name"].get<std::string>();
    }

    void drawAnimationSelector(Panel& p, TitleDesignerState& state, const ta::TitleAnimation* a){
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("##animation", p.animation.empty() ? "No Animation" : p.animation.c_str())){
            for (const ta::TitleAnimation& it : p.set.animations) if (ImGui::Selectable(it.name.c_str(), it.name == p.animation)) p.animation = it.name;
            ImGui::Separator();
            if (ImGui::Selectable(ICON_FA_PLUS " New Animation")) newAnimation(p, state);
            if (a && ImGui::Selectable("Duplicate")){
                tt::Json op = opBase("duplicate_animation", a->name);
                op["name"] = uniqueTitleAnimationName(p.set, a->name);
                commit(p, state, tt::Json::array({op}), "Duplicate Animation");
            }
            if (a && ImGui::Selectable("Rename...")) ImGui::OpenPopup("##renameanim");
            if (a && ImGui::Selectable("Delete")){
                commit(p, state, tt::Json::array({opBase("remove_animation", a->name)}), "Delete Animation");
                p.animation.clear();
            }
            if (a && ImGui::Selectable("Save as Preset...")) ImGui::OpenPopup("##savepreset");
            ImGui::EndCombo();
        }
        // the name popups sit at the toolbar's id scope
        static char rename[64] = {0};
        if (ImGui::BeginPopup("##renameanim")){
            if (ImGui::IsWindowAppearing()) std::snprintf(rename, sizeof(rename), "%s", p.animation.c_str());
            ImGui::InputText("Name", rename, sizeof(rename));
            if (ImGui::Button("Rename") && a){
                tt::Json op = opBase("set_animation", a->name);
                op["values"]["name"] = std::string(rename);
                if (commit(p, state, tt::Json::array({op}), "Rename Animation")) p.animation = rename;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        static char presetName[64] = {0};
        if (ImGui::BeginPopup("##savepreset")){
            ImGui::InputText("Preset Name", presetName, sizeof(presetName));
            if (ImGui::Button("Save") && a){
                tt::Json preset;
                std::string error, path;
                if (titleAnimationToPreset(state.shown(), *a, presetName, presetName, preset, error) && saveTitleAnimPreset(preset, "user", path, error)){
                    TitleAnimPresets::reload();
                    p.status = "Saved preset: " + path;
                }else{
                    p.status = error;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // §6.9: what the panel's toggles chose, written through the window's settings
    void savePreferences(Panel& p){
        if (!p.settings) return;
        p.settings->onionSkin = p.onion;
        p.settings->animationPreviewAs = p.previewAs;
        p.settings->animationPreviewSounds = !p.muted;
        p.settings->save();
    }

    // the next item beside the last, or on a new row when `width` does not fit (a narrow window)
    void flow(float width){
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
    }

    void drawToolbar(Panel& p, TitleDesignerState& state, const ta::TitleAnimation* a){
        drawAnimationSelector(p, state, a);
        ImGui::SameLine();
        if (ImGui::Button(p.playing ? ICON_FA_PAUSE : ICON_FA_PLAY) && a){
            p.playing = !p.playing;
            p.previewing = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_STOP)) stopPreview(p);
        ImGui::SameLine();
        ImGui::Checkbox("Loop", &p.loopPreview);
        const int fps = a ? fpsOf(p, *a) : 30;
        auto step = [&](const char* label, double delta){
            ImGui::SameLine();
            if (ImGui::SmallButton(label) && a){
                p.time = std::max(0.0, snapTitleKeyTime(p.time + delta, fps));
                p.previewing = true;
            }
        };
        step(ICON_FA_BACKWARD_STEP "##start", -1e6);
        step(ICON_FA_CHEVRON_LEFT "##prevframe", -1.0 / fps);
        step(ICON_FA_CHEVRON_RIGHT "##nextframe", 1.0 / fps);
        step(ICON_FA_FORWARD_STEP "##end", 1e6);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        double shown = p.frames ? std::round(p.time * fps) : p.time;
        if (ImGui::InputDouble("##time", &shown, 0.0, 0.0, p.frames ? "%.0f" : "%.3f")){
            p.time = std::max(0.0, p.frames ? shown / fps : shown);
            p.previewing = true;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Frames", &p.frames);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, p.record ? ImVec4(0.75f, 0.15f, 0.15f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        if (ImGui::Button(ICON_FA_CIRCLE " Record") && a){
            p.record = !p.record;
            if (p.record) p.previewing = true;
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Add Preset")) ImGui::OpenPopup("##presets");
        if (ImGui::BeginPopup("##presets")){
            drawPresetMenu(p, state);
            ImGui::EndPopup();
        }else if (!p.hoveredPreset.empty()){
            state.clearPreview();
            p.hoveredPreset.clear();
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("Onion", &p.onion)) savePreferences(p);
        if (ImGui::BeginPopupContextItem("##onionoptions")){
            if (p.settings){
                bool changed = ImGui::DragInt("Before", &p.settings->onionBefore, 0.1f, 0, 8);
                changed = ImGui::DragInt("After", &p.settings->onionAfter, 0.1f, 0, 8) || changed;
                changed = ImGui::SliderFloat("Opacity", &p.settings->onionOpacity, 0.05f, 1.0f, "%.2f") || changed;
                if (changed) savePreferences(p);
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Curves", &p.curves);
        flow(150.0f);
        ImGui::SetNextItemWidth(110.0f);
        static const char* const modes[] = {"Exact", "Baked", "Realtime", "Glyph Atlas"};
        static const char* const ids[] = {"exact", "baked", "realtime", "glyph_atlas"};
        int current = 0;
        for (int i = 0; i < 4; i++) if (p.previewAs == ids[i]) current = i;
        if (ImGui::Combo("##previewas", &current, modes, 4)){
            p.previewAs = ids[current];
            savePreferences(p);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(p.muted ? ICON_FA_VOLUME_XMARK "##mute" : ICON_FA_VOLUME_HIGH "##mute")){
            p.muted = !p.muted;
            savePreferences(p);
        }
        if (a){
            // the cost badge: colour by the mode's verdict (R44); a click runs the animation checks
            const TitleArtRenderMode mode = p.previewAs == "baked" ? TitleArtRenderMode::BAKED : p.previewAs == "glyph_atlas" ? TitleArtRenderMode::GLYPH_ATLAS : TitleArtRenderMode::REALTIME;
            TitleAnimRenderer* renderer = rendererOf(p, state);
            const tt::AnimationPlan* plan = renderer->plan(mode).find(p.animation);
            const ta::CompiledTitleAnimation* compiled = renderer->library().find(p.animation);
            if (plan && compiled){
                TitleAnimCostInputs inputs;
                // what one re-render costs, planned once per document change
                if (mode == TitleArtRenderMode::REALTIME && plan->rerender){
                    if (p.renderUs < 0.0) p.renderUs = titleRenderEstimateUs(TitleArtRenderHost::editor().resolver(state.project()), state.shown(), 1.0f, tt::TitleRectI());
                    inputs.renderUs = p.renderUs;
                }
                const std::string summary = estimateAnimCost(state.shown(), *compiled, *plan, inputs).summary();
                flow(ImGui::CalcTextSize(summary.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f);
                const ImVec4 color = plan->tracks.empty() ? ImVec4(0.45f, 0.75f, 0.45f, 1.0f) : ImVec4(0.8f, 0.7f, 0.3f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                if (ImGui::SmallButton(summary.c_str())) p.status = "Run check_title with checks=[\"anim\"] for details";
                ImGui::PopStyleColor();
            }
        }
    }

    void drawSettings(Panel& p, TitleDesignerState& state, const ta::TitleAnimation& a){
        auto setValue = [&](const char* key, const tt::Json& value, const char* undo){
            tt::Json op = opBase("set_animation", a.name);
            op["values"][key] = value;
            commit(p, state, tt::Json::array({op}), undo);
        };
        bool autoDuration = a.duration < 0.0f;
        if (ImGui::Checkbox("Auto", &autoDuration)) setValue("duration", autoDuration ? -1.0 : p.duration, "Duration");
        ImGui::SameLine();
        if (!autoDuration){
            float d = a.duration;
            ImGui::SetNextItemWidth(55.0f);
            if (ImGui::InputFloat("Duration", &d, 0.0f, 0.0f, "%.2f") && ImGui::IsItemDeactivatedAfterEdit()) setValue("duration", d, "Duration");
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(90.0f);
        static const char* const wraps[] = {"Once", "Loop", "Ping Pong"};
        int wrap = (int)a.wrap;
        if (ImGui::Combo("Wrap", &wrap, wraps, 3)) setValue("wrap", ta::animWrapName((ta::AnimWrap)wrap), "Wrap");
        ImGui::SameLine();
        if (a.wrap != ta::AnimWrap::Once){
            float loopStart = a.loopStart;
            ImGui::SetNextItemWidth(55.0f);
            if (ImGui::InputFloat("Loop Start", &loopStart, 0.0f, 0.0f, "%.2f") && ImGui::IsItemDeactivatedAfterEdit()) setValue("loop_start", loopStart, "Loop Start");
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(100.0f);
        static const char* const roles[] = {"Custom", "Intro", "Idle", "Emphasis", "Win", "Outro"};
        int role = (int)a.role;
        if (ImGui::Combo("Role", &role, roles, 6)) setValue("role", ta::animRoleName((ta::AnimRole)role), "Role");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::BeginCombo("Next", a.next.empty() ? "None" : a.next.c_str())){
            if (ImGui::Selectable("None", a.next.empty())) setValue("next", "", "Next");
            for (const ta::TitleAnimation& other : p.set.animations)
                if (other.name != a.name && ImGui::Selectable(other.name.c_str(), other.name == a.next)) setValue("next", other.name, "Next");
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        bool overlay = a.overlay;
        if (ImGui::Checkbox("Overlay", &overlay)) setValue("overlay", overlay, "Overlay");
        ImGui::SameLine();
        float speed = a.speed;
        ImGui::SetNextItemWidth(50.0f);
        if (ImGui::InputFloat("Speed", &speed, 0.0f, 0.0f, "%.2f") && ImGui::IsItemDeactivatedAfterEdit()) setValue("speed", speed, "Speed");
        flow(120.0f);
        ImGui::SetNextItemWidth(50.0f);
        float flipFps = a.flipbook.fps;
        if (ImGui::InputFloat("Flip FPS", &flipFps, 0.0f, 0.0f, "%.0f") && ImGui::IsItemDeactivatedAfterEdit()) setValue("flipbook.fps", flipFps, "Flipbook FPS");
        flow(130.0f);
        ImGui::SetNextItemWidth(50.0f);
        float flipScale = a.flipbook.scale;
        if (ImGui::InputFloat("Flip Scale", &flipScale, 0.0f, 0.0f, "%.2f") && ImGui::IsItemDeactivatedAfterEdit()) setValue("flipbook.scale", flipScale, "Flipbook Scale");
        flow(110.0f);
        bool prerender = a.rerender == ta::RerenderPolicy::Prerender;
        if (ImGui::Checkbox("Prerender", &prerender)) setValue("rerender", prerender ? "prerender" : "live", "Realtime Policy");
    }

    void drawPanel(TitleDesignerState& state){
        Panel& p = panel();
        refresh(p, state);
        ImGui::BeginChild("##titleanimpanel", ImVec2(0, 0), false);
        p.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        const ta::TitleAnimation* a = current(p);
        drawToolbar(p, state, a);
        a = current(p);     // a toolbar action can replace the animation set
        if (!a){
            ImGui::TextDisabled("%s", p.set.animations.empty() ? "No animations yet: pick New Animation in the dropdown, or an Add Preset." : "Select an animation.");
            if (!p.status.empty()) ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1.0f), "%s", p.status.c_str());
            ImGui::EndChild();
            return;
        }
        advance(p, state, *a);
        drawSettings(p, state, *a);
        a = current(p);
        if (!a){
            ImGui::EndChild();
            return;
        }
        if (!p.status.empty()) ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1.0f), "%s", p.status.c_str());
        const ImVec2 region = ImGui::GetContentRegionAvail();
        // the view fits the animation until the user zooms
        const float width = std::max(100.0f, region.x - TRACK_COLUMN);
        p.secondsPerPixel = std::min(p.secondsPerPixel, std::max(p.duration, 1.0) / width);
        if (p.curves) drawCurves(p, state, *a, region);
        else drawDopesheet(p, state, *a, region);
        ImGui::EndChild();
    }

    // Record (R45): the Designer's sets of animatable fields become keys
    tt::Json recordRouter(const tt::Json& ops){
        Panel& p = panel();
        if (!p.record || !p.state || !p.state->isOpen()) return ops;
        // the committed document: a gesture stages from it each frame, and the track the last
        // stage made must not look like one that was already there
        const tt::TitleArtDoc& doc = p.state->committed();
        const ta::TitleAnimationSet set = tt::readAnimationSet(doc);
        const ta::TitleAnimation* a = set.find(p.animation);
        if (!a) return ops;
        TitleRecordResult out = titleRecordRewrite(doc, *a, p.time, ops);
        if (!out.stepName.empty()){
            if (p.state->gestureActive()) p.state->setGestureName(out.stepName);
            else p.state->setRoutedStepName(out.stepName);
        }
        if (!out.notKeyed.empty()) p.status = "Record: changed the document (not animatable): " + out.notKeyed.front();
        return out.ops;
    }

}

void jasperengine::editor::titledesigner::registerTitleAnimationPanel(TitleDesignerSettings* settings){
    Panel& shared = panel();
    shared.settings = settings;
    if (settings){
        shared.onion = settings->onionSkin;
        shared.muted = !settings->animationPreviewSounds;
        shared.previewAs = settings->animationPreviewAs;
    }
    registerTitleAnimOps();
    registerTitleAnimPresetOp();
    registerTitleAnimChecks();
    static bool done = false;
    if (done) return;
    done = true;
    PanelDef def;
    def.id = "animation";
    def.title = "Animation";
    def.region = PanelDef::Region::Bottom;
    def.draw = drawPanel;
    def.visibleByDefault = [](const TitleDesignerState& state){ return state.isOpen() && tt::hasAnimations(state.shown()); };
    addPanel(std::move(def));
    setCanvasPoseSource(CanvasPoseSource{[](const TitleDesignerState&){ return poseActive(panel()); }, drawCanvasPose});
    setEditRouter(recordRouter);
    addCheckSource("animation", [](const TitleDesignerState& state){
        std::vector<CheckFinding> out;
        if (!tt::hasAnimations(state.shown())) return out;
        std::vector<const CheckDef*> checks;
        std::string error;
        std::vector<std::string> valid;
        if (!TitleArtChecks::select({"anim"}, checks, error, valid)) return out;
        // the document checks only: this runs inside a frame, the pixel checks are check_title's
        checks.erase(std::remove_if(checks.begin(), checks.end(), [](const CheckDef* c){ return c->kind != TitleCheckKind::Document; }), checks.end());
        TitleCheckContext context(state.shown(), std::string());
        const TitleCheckReport report = TitleArtChecks::run(checks, context);
        for (const TitleCheckResult& r : report.checks){
            if (r.status != TitleCheckStatus::Warn && r.status != TitleCheckStatus::Fail) continue;
            CheckFinding f;
            f.id = r.id;
            f.status = r.status == TitleCheckStatus::Fail ? "FAIL" : "WARN";
            f.message = r.message;
            f.fix = r.fix;
            out.push_back(f);
        }
        return out;
    });
}

bool jasperengine::editor::titledesigner::previewTitleAnimation(const std::string& name, double time, std::string* error){
    Panel& p = panel();
    if (time < 0.0){
        stopPreview(p);
        return true;
    }
    if (!p.state || !p.state->isOpen()){
        if (error) *error = "the Animation panel has not drawn yet: open the document and open the panel first";
        return false;
    }
    refresh(p, *p.state);
    if (findTitleAnimation(p.set, name) < 0){
        std::string names;
        for (const ta::TitleAnimation& a : p.set.animations) names += (names.empty() ? "" : ", ") + a.name;
        if (error) *error = "no animation '" + name + "' (it has: " + names + ")";
        return false;
    }
    p.animation = name;
    p.time = time;
    p.playing = false;
    p.previewing = true;
    return true;
}

tt::Json jasperengine::editor::titledesigner::titleAnimationPanelState(){
    Panel& p = panel();
    tt::Json out = tt::Json::object();
    out["animation"] = p.animation;
    out["time"] = p.time;
    out["playing"] = p.playing;
    out["previewing"] = p.previewing;
    out["record"] = p.record;
    out["curves"] = p.curves;
    out["selected_keys"] = p.keySelection.size();
    out["preview_active"] = p.state && p.state->previewActive();
    out["status"] = p.status;
    out["timeline"] = {{"x", p.geometry.x}, {"ruler_y", p.geometry.rulerY}, {"ruler_h", p.geometry.rulerH}, {"rows_y", p.geometry.rowsY}, {"row_h", p.geometry.rowH},
                       {"view_start", p.viewStart}, {"seconds_per_pixel", p.secondsPerPixel}};
    if (p.curves){
        const auto& v = p.curveEditor.getView();
        out["curve_view"] = {{"t0", v.t0}, {"t1", v.t1}, {"v0", v.v0}, {"v1", v.v1}, {"x0", v.x0}, {"y0", v.y0}, {"x1", v.x1}, {"y1", v.y1}};
    }
    return out;
}

bool jasperengine::editor::titledesigner::titleAnimationPanelFocused(){
    return panel().focused;
}

bool jasperengine::editor::titledesigner::runTitleAnimationCommand(const std::string& command){
    Panel& p = panel();
    TitleDesignerState* state = p.state;
    if (!state || !state->isOpen()) return false;
    refresh(p, *state);
    const ta::TitleAnimation* a = current(p);
    const int fps = a ? fpsOf(p, *a) : 30;
    if (command == "titleAnimation.playPause"){
        if (!a) return false;
        p.playing = !p.playing;
        p.previewing = true;
    }else if (command == "titleAnimation.stop"){
        stopPreview(p);
    }else if (command == "titleAnimation.record"){
        p.record = !p.record;
        if (p.record) p.previewing = true;
    }else if (command == "titleAnimation.toggleCurves"){
        p.curves = !p.curves;
    }else if (command == "titleAnimation.previousFrame" || command == "titleAnimation.nextFrame"){
        if (!a) return false;
        p.time = std::max(0.0, snapTitleKeyTime(p.time + (command == "titleAnimation.nextFrame" ? 1.0 : -1.0) / fps, fps));
        p.previewing = true;
    }else if (command == "titleAnimation.previousKey" || command == "titleAnimation.nextKey"){
        if (!a) return false;
        const bool next = command == "titleAnimation.nextKey";
        double best = p.time;
        bool found = false;
        for (const ta::TitleTrack& t : a->tracks){
            for (const ta::TitleKey& k : t.keys){
                const double time = k.key.time;
                if (next ? (time > p.time + 1e-4 && (!found || time < best)) : (time < p.time - 1e-4 && (!found || time > best))){
                    best = time;
                    found = true;
                }
            }
        }
        if (!found) return false;
        p.time = best;
        p.previewing = true;
    }else if (command == "titleAnimation.addKey" || command == "titleAnimation.addKeyModified"){
        if (!a || p.keySelection.empty()) return false;
        std::set<int> tracks;
        for (const TitleKeyRef& r : p.keySelection) tracks.insert(r.track);
        tt::Json ops = tt::Json::array();
        for (int track : tracks){
            tt::Json op = opBase("set_key", a->name);
            op["layer"] = titleLayerRef(a->tracks[(size_t)track].layer);
            op["field"] = a->tracks[(size_t)track].field;
            op["time"] = p.time;
            ops.push_back(op);
        }
        commit(p, *state, ops, "Add Key");
    }else if (command == "titleAnimation.delete"){
        if (!a) return false;
        commit(p, *state, titleDeleteKeysOps(*a, p.keySelection), "Delete Keys");
        p.keySelection.clear();
    }else if (command == "titleAnimation.copy"){
        if (!a) return false;
        p.clipboard = titleCopyKeys(*a, p.keySelection);
    }else if (command == "titleAnimation.paste"){
        if (!a) return false;
        commit(p, *state, titlePasteKeysOps(*a, p.clipboard, p.time), "Paste Keys");
    }else if (command == "titleAnimation.duplicate"){
        if (!a) return false;
        commit(p, *state, titleDuplicateKeysOps(*a, p.keySelection, 1.0 / fps), "Duplicate Keys");
    }else if (command == "titleAnimation.frameSelected" || command == "titleAnimation.frameAll"){
        p.viewStart = 0.0;
        p.secondsPerPixel = 1e9;    // the draw's fit clamps it
    }else if (command == "titleAnimation.addPreset"){
        return false;   // Add Preset is the toolbar's menu
    }else if (command == "titleAnimation.saveAsPreset"){
        return false;   // in the animation dropdown
    }else{
        return false;
    }
    return true;
}
