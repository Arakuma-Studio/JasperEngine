// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's curves view (animation clips spec 4.4, 9.R50): the Dopesheet / Curves
// toggle's second view. Left, the curves of the open clip's bound channels by component (colour,
// visibility toggle, label); right, the graph: a value axis with nice ticks, every visible curve
// sampled with the runtime sampler (clipcurves, AnimationWindowClipCurves.h), key points,
// read-only tangents of the keys that carry them, the playhead. Key points are selected like the
// dopesheet's keys (the same selection), dragged in time (frame snapped) and value through the
// controller's key drag API (one merge key per drag, keys passed over are never lost); the key
// menu (ease presets) opens on a key or a curve segment; F frames the selected keys, A all.

#include "AnimationWindowClipCurves.h"
#include "AnimationWindowClip.h"
#include "AnimationWindowClipUtil.h"

#include "imgui_internal.h"
#include "external/IconsFontAwesome6.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/KeyframeSampling.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::clipui;

// ---- the pure part (clipcurves) ----

size_t clipcurves::componentCount(const ClipTrackData& track){
    switch (track.channel){
        case ClipChannel::Position:
        case ClipChannel::Rotation:
        case ClipChannel::Scale:
            return 3;
        case ClipChannel::Morph: {
            size_t width = 0;
            for (const ClipKey& key : track.keys) width = std::max(width, key.value.size());
            return width;
        }
        case ClipChannel::Lattice:
            return track.points.size() * 3;
        case ClipChannel::Property:
            break;
    }
    return 0;
}

std::string clipcurves::componentLabel(const ClipTrackData& track, size_t component, const std::vector<std::string>& morphNames){
    static const char* const axes[3] = {"x", "y", "z"};
    switch (track.channel){
        case ClipChannel::Position:
        case ClipChannel::Rotation:
        case ClipChannel::Scale:
            return component < 3 ? axes[component] : std::string();
        case ClipChannel::Morph:
            if (component < morphNames.size() && !morphNames[component].empty()) return morphNames[component];
            return "Weight " + std::to_string(component);
        case ClipChannel::Lattice: {
            const size_t point = component / 3;
            const int index = point < track.points.size() ? track.points[point] : (int)point;
            return "Point " + std::to_string(index) + " " + axes[component % 3];
        }
        case ClipChannel::Property:
            break;
    }
    return std::string();
}

Vector3 clipcurves::nearestEuler(const Vector3& euler, const Vector3& reference){
    auto near = [](float value, float target){
        return value + 360.0f * std::round((target - value) / 360.0f);
    };
    auto distance = [&reference](const Vector3& e){
        return std::fabs(e.x - reference.x) + std::fabs(e.y - reference.y) + std::fabs(e.z - reference.z);
    };
    const Vector3 same(near(euler.x, reference.x), near(euler.y, reference.y), near(euler.z, reference.z));
    // the same rotation with the middle angle mirrored: (x + 180, 180 - y, z + 180)
    const Vector3 other(near(euler.x + 180.0f, reference.x), near(180.0f - euler.y, reference.y), near(euler.z + 180.0f, reference.z));
    return distance(other) + 1e-3f < distance(same) ? other : same;
}

std::vector<Vector3> clipcurves::keyEulers(const ClipTrackData& track){
    std::vector<Vector3> out;
    if (track.channel != ClipChannel::Rotation) return out;
    out.reserve(track.keys.size());
    for (size_t k = 0; k < track.keys.size(); k++){
        const Vector3 euler = eulerOf(track.keys[k].value);
        out.push_back(k == 0 ? euler : nearestEuler(euler, out.back()));
    }
    return out;
}

bool clipcurves::sampleCurves(const AnimationClip::Track& runtime, const ClipTrackData& track, const std::vector<float>& times,
                              std::vector<std::vector<float>>& out){
    out.clear();
    if (track.channel == ClipChannel::Property || track.keys.empty() || !runtime.isSampled()) return false;
    const size_t count = componentCount(track);
    out.reserve(times.size());
    const bool rotation = track.channel == ClipChannel::Rotation;
    const std::vector<Vector3> eulers = rotation ? keyEulers(track) : std::vector<Vector3>();
    anim::KeyCursor cursor;
    // rotation continuity: the segment of the previous sample and its Euler
    long previousSegment = -2;
    Vector3 previous(0, 0, 0);
    for (float time : times){
        anim::TrackSample sample;
        std::vector<float> row(count, 0.0f);
        if (anim::sampleTrack(runtime, time, sample, &cursor) && sample.valid){
            if (rotation){
                // the key at or before the time (-1 before the first key)
                const auto after = std::upper_bound(track.keys.begin(), track.keys.end(), time + kKeyEpsilon,
                                                    [](float t, const ClipKey& k){ return t < k.time; });
                const long segment = (long)(after - track.keys.begin()) - 1;
                const size_t keyIndex = (size_t)std::max(0L, segment);
                Vector3 euler;
                if (segment >= 0 && std::fabs(track.keys[keyIndex].time - time) <= kKeyEpsilon){
                    // at a key: the key's continuous Euler (the curve passes through its point)
                    euler = eulers[keyIndex];
                }else{
                    std::vector<float> xyzw(4);
                    quaternionToXYZW(sample.rotation, xyzw.data());
                    const Vector3 reference = segment == previousSegment ? previous : eulers[keyIndex];
                    euler = nearestEuler(eulerOf(xyzw), reference);
                }
                previousSegment = segment;
                previous = euler;
                row = {euler.x, euler.y, euler.z};
            }else{
                const std::vector<float> flat = flatten(sample);
                for (size_t c = 0; c < count && c < flat.size(); c++) row[c] = flat[c];
            }
        }
        out.push_back(row);
    }
    return true;
}

std::vector<float> clipcurves::keyValues(const ClipTrackData& track, size_t key, const std::vector<Vector3>& eulers){
    if (key >= track.keys.size()) return {};
    if (track.channel == ClipChannel::Rotation){
        const Vector3 euler = key < eulers.size() ? eulers[key] : eulerOf(track.keys[key].value);
        return {euler.x, euler.y, euler.z};
    }
    std::vector<float> values = track.keys[key].value;
    values.resize(componentCount(track), 0.0f);
    return values;
}

float clipcurves::niceStep(float range, float pixels, float minSpacing){
    if (!(range > 0.0f) || !std::isfinite(range) || !(pixels > 0.0f)) return 1.0f;
    const float raw = range * std::max(1.0f, minSpacing) / pixels;
    const float magnitude = std::pow(10.0f, std::floor(std::log10(raw)));
    for (float m : {1.0f, 2.0f, 5.0f, 10.0f}){
        if (m * magnitude >= raw * 0.9999f) return m * magnitude;
    }
    return 10.0f * magnitude;
}

std::vector<float> clipcurves::ticks(float min, float max, float step){
    std::vector<float> out;
    if (!(step > 0.0f) || !std::isfinite(min) || !std::isfinite(max) || max < min) return out;
    const double first = std::ceil((double)min / step - 1e-6);
    const double last = std::floor((double)max / step + 1e-6);
    if (last - first > 1000.0) return out;
    for (double i = first; i <= last; i += 1.0){
        float value = (float)(i * step);
        if (std::fabs(value) < step * 1e-4f) value = 0.0f;
        out.push_back(value);
    }
    return out;
}

// ---- the view ----

namespace {

    std::string curveKey(const ClipTrackData& track, int component){
        return rowKey(track) + "#" + std::to_string(component);
    }

    // x red, y green, z blue (from the Theme); blend shapes cycle through the Theme's accents
    ImVec4 curveColor(const ClipTrackData& track, size_t component){
        if (track.channel == ClipChannel::Morph){
            const ImVec4 palette[] = {Theme::Colors::WarningText, Theme::Colors::ChildSceneText, Theme::Colors::SubSelectionText,
                                      Theme::Colors::PrefabText, Theme::Colors::PhysicsAccent, Theme::Colors::ErrorText,
                                      mix(Theme::Colors::DisabledGreenText, ImVec4(1, 1, 1, 1), 0.2f), Theme::Colors::LogIcon};
            return palette[component % (sizeof(palette) / sizeof(palette[0]))];
        }
        switch (component % 3){
            case 0: return Theme::Colors::ErrorText;
            case 1: return mix(Theme::Colors::DisabledGreenText, ImVec4(1, 1, 1, 1), 0.2f);
            default: return Theme::Colors::PrefabText;
        }
    }

    // a value label with as many decimals as the step needs
    std::string formatValue(float value, float step){
        const int decimals = std::clamp((int)std::ceil(-std::log10(std::max(step, 1e-6f)) - 1e-4f), 0, 5);
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", decimals, value);
        return text;
    }

    struct CurvePoint{
        size_t track = 0;
        size_t key = 0;
        int component = -1;
        float time = 0.0f;
        float value = 0.0f;
    };

}

bool AnimationWindowClip::curveVisible(const ClipTrackData& track, int component) const{
    return !hiddenCurves.count(curveKey(track, component));
}

void AnimationWindowClip::setCurveVisible(const ClipTrackData& track, int component, bool visible){
    if (visible) hiddenCurves.erase(curveKey(track, component));
    else hiddenCurves.insert(curveKey(track, component));
}

const std::vector<Vector3>& AnimationWindowClip::trackEulers(size_t trackIndex) const{
    static const std::vector<Vector3> none;
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || trackIndex >= doc->data().tracks.size()) return none;
    // per document revision (a rotation track's continuous key Eulers)
    if (doc != curveEulerDocument.lock() || doc->revision() != curveEulerRevision){
        curveEulerDocument = doc;
        curveEulerRevision = doc->revision();
        curveEulerCache.assign(doc->data().tracks.size(), {});
        curveEulerValid.assign(doc->data().tracks.size(), 0);
    }
    if (!curveEulerValid[trackIndex]){
        curveEulerCache[trackIndex] = clipcurves::keyEulers(doc->data().tracks[trackIndex]);
        curveEulerValid[trackIndex] = 1;
    }
    return curveEulerCache[trackIndex];
}

void AnimationWindowClip::frameCurveValues(bool selectedOnly){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    refreshSelection();
    const ClipData& data = doc->data();
    float low = std::numeric_limits<float>::max(), high = -std::numeric_limits<float>::max();
    auto take = [&](float v){
        if (!std::isfinite(v)) return;
        low = std::min(low, v);
        high = std::max(high, v);
    };
    if (selectedOnly && !selectedRefs.empty()){
        for (const AnimationClipController::KeyRef& ref : selectedRefs){
            const ClipTrackData& track = data.tracks[ref.track];
            const std::vector<float> values = clipcurves::keyValues(track, ref.key, trackEulers(ref.track));
            for (size_t c = 0; c < values.size(); c++){
                if (curveVisible(track, (int)c)) take(values[c]);
            }
        }
    }else{
        // every visible curve over the clip (overshooting eases included)
        std::shared_ptr<const AnimationClip> clip = doc->clip();
        const float length = std::max(data.effectiveLength(), 1e-3f);
        std::vector<float> times;
        for (int i = 0; i <= 256; i++) times.push_back(length * (float)i / 256.0f);
        for (size_t t = 0; t < data.tracks.size() && clip; t++){
            const ClipTrackData& track = data.tracks[t];
            const int index = clip->findTrack(track.path, track.channel);
            if (index < 0) continue;
            std::vector<float> all = times;
            for (const ClipKey& key : track.keys) all.push_back(key.time);
            std::sort(all.begin(), all.end());
            std::vector<std::vector<float>> rows;
            if (!clipcurves::sampleCurves(clip->getTracks()[(size_t)index], track, all, rows)) continue;
            for (const std::vector<float>& row : rows){
                for (size_t c = 0; c < row.size(); c++){
                    if (curveVisible(track, (int)c)) take(row[c]);
                }
            }
        }
    }
    if (low > high){
        low = -1.0f;
        high = 1.0f;
    }
    if (high - low < 1e-4f){
        const float pad = std::max(1.0f, std::fabs(low) * 0.1f);
        low -= pad;
        high += pad;
    }
    const float pad = (high - low) * 0.08f;
    curveMin = low - pad;
    curveMax = high + pad;
}

bool AnimationWindowClip::beginCurveDrag(int trackIndex, int component, std::string* error){
    refreshSelection();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || selectedRefs.empty()){
        if (error) *error = "no keys selected";
        return false;
    }
    const ClipData& data = doc->data();
    if (trackIndex < 0 || trackIndex >= (int)data.tracks.size()){
        if (error) *error = "no such track";
        return false;
    }
    const ClipTrackData& grabbed = data.tracks[(size_t)trackIndex];
    // the value moves on the grabbed component: of the keys of one property (blend shapes and
    // lattices: of one track, their components differ), of one rotation key (an Euler change is
    // not the same quaternion offset for two keys); otherwise only the time moves
    curveValueLocked = component < 0 || component >= (int)clipcurves::componentCount(grabbed);
    for (const AnimationClipController::KeyRef& ref : selectedRefs){
        const ClipTrackData& track = data.tracks[ref.track];
        if (track.channel != grabbed.channel) curveValueLocked = true;
        if ((grabbed.channel == ClipChannel::Morph || grabbed.channel == ClipChannel::Lattice) && ref.track != (size_t)trackIndex) curveValueLocked = true;
    }
    if (grabbed.channel == ClipChannel::Rotation && selectedRefs.size() != 1) curveValueLocked = true;
    curveDragWidth = 0;
    curveDragQuaternion.clear();
    for (const AnimationClipController::KeyRef& ref : selectedRefs){
        if (ref.track != (size_t)trackIndex) continue;
        const ClipKey& key = data.tracks[ref.track].keys[ref.key];
        curveDragWidth = std::max(curveDragWidth, key.value.size());
        if (grabbed.channel == ClipChannel::Rotation){
            curveDragQuaternion = key.value;
            const std::vector<Vector3>& eulers = trackEulers(ref.track);
            curveDragEuler = ref.key < eulers.size() ? eulers[ref.key] : eulerOf(key.value);
        }
    }
    if (grabbed.channel == ClipChannel::Rotation && curveDragQuaternion.size() != 4) curveValueLocked = true;
    if (!controller().beginKeyDrag(selectedRefs, error)) return false;
    curveGrabTrack = trackIndex;
    curveGrabComponent = component;
    curveDragTime = 0.0f;
    curveDragValue = 0.0f;
    float earliest = kMaxTime;
    for (const KeyId& id : selection) earliest = std::min(earliest, id.time);
    dragMinTime = earliest;
    drag = Drag::CurveKeys;
    return true;
}

bool AnimationWindowClip::updateCurveDrag(float rawTimeDelta, float valueDelta, std::string* error){
    if (drag != Drag::CurveKeys) return false;
    // frame snapped: the grabbed key lands on a frame; the earliest key stops at 0
    float delta = snapToFrame(dragGrabTime + rawTimeDelta) - dragGrabTime;
    delta = std::max(delta, -dragMinTime);
    const float value = curveValueLocked || !std::isfinite(valueDelta) ? 0.0f : valueDelta;
    if (std::fabs(delta - curveDragTime) < 1e-6f && std::fabs(value - curveDragValue) < 1e-7f) return true;
    std::vector<float> offset;
    if (value != 0.0f){
        std::shared_ptr<AnimationClipDocument> doc = document();
        const ClipChannel channel = doc && curveGrabTrack >= 0 && curveGrabTrack < (int)doc->data().tracks.size()
            ? doc->data().tracks[(size_t)curveGrabTrack].channel : ClipChannel::Position;
        if (channel == ClipChannel::Rotation){
            // the grabbed key's Euler component changed: the quaternion offset from its base
            Vector3 euler = curveDragEuler;
            if (curveGrabComponent == 0) euler.x += value;
            if (curveGrabComponent == 1) euler.y += value;
            if (curveGrabComponent == 2) euler.z += value;
            const std::vector<float> target = xyzwOf(euler);
            offset.resize(4);
            for (size_t i = 0; i < 4; i++) offset[i] = target[i] - curveDragQuaternion[i];
        }else{
            offset.assign(std::max(curveDragWidth, (size_t)curveGrabComponent + 1), 0.0f);
            offset[(size_t)curveGrabComponent] = value;
        }
    }
    std::vector<AnimationClipController::KeyRef> moved;
    if (!controller().updateKeyDrag(delta, offset.empty() ? nullptr : &offset, &moved, error)){
        controller().endKeyDrag();
        drag = Drag::None;
        return false;
    }
    curveDragTime = delta;
    curveDragValue = value;
    setSelectionFromRefs(moved);
    return true;
}

nlohmann::json AnimationWindowClip::curvesJson() const{
    nlohmann::json out = nlohmann::json::array();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return out;
    const ClipData& data = doc->data();
    for (size_t t = 0; t < data.tracks.size(); t++){
        const ClipTrackData& track = data.tracks[t];
        const std::vector<std::string> names = track.channel == ClipChannel::Morph ? componentLabels(t) : std::vector<std::string>();
        const std::vector<Vector3>& eulers = trackEulers(t);
        const size_t count = clipcurves::componentCount(track);
        for (size_t c = 0; c < count; c++){
            nlohmann::json values = nlohmann::json::array();
            for (size_t k = 0; k < track.keys.size() && k < 512; k++){
                const std::vector<float> keyed = clipcurves::keyValues(track, k, eulers);
                values.push_back(c < keyed.size() ? keyed[c] : 0.0f);
            }
            out.push_back({{"track", t}, {"component", c}, {"label", clipcurves::componentLabel(track, c, names)},
                           {"visible", curveVisible(track, (int)c)}, {"key_values", values}});
        }
    }
    return out;
}

void AnimationWindowClip::drawCurves(SceneProject* sceneProject, ImVec2 origin, float labelW, float width, float height){
    (void)sceneProject;
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    const ClipData& data = doc->data();
    const SheetMetrics m = sheetMetrics();
    ImGuiIO& io = ImGui::GetIO();
    const bool readOnly = doc->isReadOnly();
    const float fps = frameRate();
    if (curveRangeClip != doc->path()){
        curveRangeClip = doc->path();
        frameCurveValues(false);
    }
    const std::vector<Row> rows = buildRows();
    auto rowOfTrack = [&rows](size_t track){
        for (size_t r = 0; r < rows.size(); r++){
            if (rows[r].kind == Row::Kind::Track && rows[r].track == (int)track) return (int)r;
        }
        return -1;
    };

    // ---- the curve list (labels, colours, visibility) ----
    ImGui::SetCursorScreenPos(origin);
    ImGui::BeginChild("##curve_list", ImVec2(labelW, height), ImGuiChildFlags_None);
    for (size_t r = 0; r < rows.size(); r++){
        const Row& row = rows[r];
        if (row.kind != Row::Kind::Track) continue;
        const ClipTrackData& track = data.tracks[(size_t)row.track];
        const size_t count = clipcurves::componentCount(track);
        if (count == 0) continue;
        ImGui::PushID(row.track);
        bool anyVisible = false;
        for (size_t k = 0; k < count; k++) anyVisible = anyVisible || curveVisible(track, (int)k);
        if (ImGui::SmallButton(anyVisible ? ICON_FA_EYE "##all" : ICON_FA_EYE_SLASH "##all")){
            for (size_t k = 0; k < count; k++) setCurveVisible(track, (int)k, !anyVisible);
        }
        ImGui::SetItemTooltip("%s", anyVisible ? "Hide this property's curves" : "Show this property's curves");
        ImGui::SameLine();
        if (row.missing) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        if (ImGui::Selectable(row.leaf.c_str(), false)){
            // the property's keys (as a click on a dopesheet label)
            std::vector<KeyId> ids;
            for (const ClipKey& key : track.keys) ids.push_back({track.path, track.channel, key.time});
            selectKeys(ids, io.KeyShift || io.KeyCtrl);
        }
        if (row.missing) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s%s%s", row.label.c_str(), row.message.empty() ? "" : "\n", row.message.c_str());
        const std::vector<std::string> names = track.channel == ClipChannel::Morph ? componentLabels((size_t)row.track) : std::vector<std::string>();
        for (size_t k = 0; k < count; k++){
            ImGui::PushID((int)k);
            const bool visible = curveVisible(track, (int)k);
            const ImVec4 colour = curveColor(track, k);
            ImGui::Indent(m.indent);
            const ImVec2 swatch = ImGui::GetCursorScreenPos();
            const float side = ImGui::GetTextLineHeight() * 0.7f;
            if (ImGui::InvisibleButton("##swatch", ImVec2(side + 4.0f * m.s, ImGui::GetTextLineHeight()))){
                setCurveVisible(track, (int)k, !visible);
            }
            ImGui::SetItemTooltip("%s", visible ? "Hide this curve" : "Show this curve");
            ImDrawList* listDraw = ImGui::GetWindowDrawList();
            const ImVec2 a(swatch.x + 2.0f * m.s, swatch.y + (ImGui::GetTextLineHeight() - side) * 0.5f);
            if (visible) listDraw->AddRectFilled(a, ImVec2(a.x + side, a.y + side), color(colour), 2.0f * m.s);
            else listDraw->AddRect(a, ImVec2(a.x + side, a.y + side), color(colour, 0.7f), 2.0f * m.s, 0, 1.0f * m.s);
            ImGui::SameLine();
            ImGui::TextColored(visible ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : Theme::Colors::SubtleText, "%s",
                               clipcurves::componentLabel(track, k, names).c_str());
            ImGui::Unindent(m.indent);
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // ---- the graph ----
    const float x0 = origin.x + labelW;
    const float x1 = origin.x + width;
    const float y0 = origin.y;
    const float y1 = origin.y + height;
    const float graphW = std::max(1.0f, x1 - x0);
    const float pad = 10.0f * m.s;
    const float graphH = std::max(1.0f, (y1 - y0) - 2.0f * pad);
    if (!(curveMax > curveMin)) curveMax = curveMin + 1.0f;
    const float pixelsPerValue = graphH / (curveMax - curveMin);
    curvePixelsPerValue = pixelsPerValue;
    curveGraphHeight = graphH;
    const float timeX = x0 + m.timePad;
    auto timeAt = [&](float x){ return (x - timeX + scrollX) / pixelsPerSecond; };
    auto xAt = [&](float t){ return timeX + t * pixelsPerSecond - scrollX; };
    auto yAt = [&](float v){ return y1 - pad - (v - curveMin) * pixelsPerValue; };
    auto valueAt = [&](float y){ return curveMin + (y1 - pad - y) / pixelsPerValue; };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    // the timeline is red while recording, blue while previewing (Unity)
    const ImU32 background = c.isRecording() ? color(Theme::Colors::ErrorText, 0.22f)
                           : c.isPreviewing() ? color(Theme::Colors::Selection, 0.28f) : color(ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), background);
    const float clipEnd = xAt(data.effectiveLength());
    if (clipEnd < x1) drawList->AddRectFilled(ImVec2(std::max(x0, clipEnd), y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 60));
    // frame lines
    const float visibleFrom = std::max(0.0f, timeAt(x0));
    const float visibleTo = std::max(visibleFrom, timeAt(x1));
    if (pixelsPerSecond / fps >= 8.0f * m.s){
        for (long f = (long)std::floor(visibleFrom * fps); f <= (long)std::ceil(visibleTo * fps); f++){
            const float x = xAt((float)f / fps);
            drawList->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(255, 255, 255, 12));
        }
    }
    // the value axis: nice ticks, grid lines, labels at the left edge
    const float step = clipcurves::niceStep(curveMax - curveMin, graphH, 26.0f * m.s);
    for (float tick : clipcurves::ticks(curveMin, curveMax, step)){
        const float y = yAt(tick);
        drawList->AddLine(ImVec2(x0, y), ImVec2(x1, y), tick == 0.0f ? IM_COL32(255, 255, 255, 45) : IM_COL32(255, 255, 255, 18));
        const std::string label = formatValue(tick, step);
        drawList->AddText(ImVec2(x0 + 3.0f * m.s, y - ImGui::GetTextLineHeight() - 1.0f * m.s), color(Theme::Colors::SubtleText), label.c_str());
    }

    // the curves: the runtime sampler every 2 px of the visible range (and at the keys)
    std::shared_ptr<const AnimationClip> clip = doc->clip();
    std::vector<float> grid;
    {
        const float stepTime = 2.0f * m.s / pixelsPerSecond;
        for (float t = visibleFrom; t <= visibleTo + stepTime; t += stepTime) grid.push_back(t);
        if (grid.size() > 4096) grid.resize(4096);
    }
    std::vector<CurvePoint> points;
    const float keyRadius = 4.0f * m.s;
    const ImVec4 selectedColour = selectedKeyColor();
    for (size_t t = 0; t < data.tracks.size() && clip; t++){
        const ClipTrackData& track = data.tracks[t];
        const size_t count = clipcurves::componentCount(track);
        bool any = false;
        for (size_t k = 0; k < count; k++) any = any || curveVisible(track, (int)k);
        if (!any || track.keys.empty()) continue;
        const int index = clip->findTrack(track.path, track.channel);
        if (index < 0) continue;
        const AnimationClip::Track& runtime = clip->getTracks()[(size_t)index];
        // the keys in the visible range, sampled with the grid (rotation continuity at keys)
        const auto first = std::lower_bound(track.keys.begin(), track.keys.end(), visibleFrom - m.keyHit / pixelsPerSecond,
                                             [](const ClipKey& k, float time){ return k.time < time; });
        const auto last = std::upper_bound(track.keys.begin(), track.keys.end(), visibleTo + m.keyHit / pixelsPerSecond,
                                           [](float time, const ClipKey& k){ return time < k.time; });
        std::vector<float> times = grid;
        for (auto it = first; it != last; ++it) times.push_back(it->time);
        std::sort(times.begin(), times.end());
        std::vector<std::vector<float>> samples;
        if (!clipcurves::sampleCurves(runtime, track, times, samples)) continue;
        std::vector<ImVec2> line(times.size());
        for (size_t k = 0; k < count; k++){
            if (!curveVisible(track, (int)k)) continue;
            for (size_t i = 0; i < times.size(); i++) line[i] = ImVec2(xAt(times[i]), yAt(samples[i][k]));
            drawList->AddPolyline(line.data(), (int)line.size(), color(curveColor(track, k), readOnly ? 0.6f : 0.95f), ImDrawFlags_None, 1.5f * m.s);
        }
        // key points (and the read-only tangents of keys that carry them)
        const std::vector<Vector3>& eulers = trackEulers(t);
        for (auto it = first; it != last; ++it){
            const size_t keyIndex = (size_t)(it - track.keys.begin());
            const std::vector<float> values = clipcurves::keyValues(track, keyIndex, eulers);
            const bool selected = isSelected(t, keyIndex);
            const float x = xAt(it->time);
            float inSlope = 0.0f, outSlope = 0.0f;
            std::vector<std::vector<float>> around;
            const float h = 0.25f / fps;
            if (it->hasTangents()) clipcurves::sampleCurves(runtime, track, {it->time - h, it->time, it->time + h}, around);
            for (size_t k = 0; k < count && k < values.size(); k++){
                if (!curveVisible(track, (int)k)) continue;
                const float y = yAt(values[k]);
                if (around.size() == 3){
                    // the curve's slopes at the key (value units per second), drawn as handles
                    inSlope = (around[1][k] - around[0][k]) / h;
                    outSlope = (around[2][k] - around[1][k]) / h;
                    const float length = 28.0f * m.s;
                    auto handle = [&](float slope, float side){
                        const ImVec2 dir(side * 1.0f, -slope * pixelsPerValue / pixelsPerSecond * side);
                        const float norm = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                        const ImVec2 end(x + dir.x / norm * length, y + dir.y / norm * length);
                        drawList->AddLine(ImVec2(x, y), end, color(Theme::Colors::SubtleText, 0.8f), 1.0f * m.s);
                        drawList->AddCircle(end, 2.5f * m.s, color(Theme::Colors::SubtleText, 0.8f));
                    };
                    if (keyIndex > 0) handle(inSlope, -1.0f);
                    if (keyIndex + 1 < track.keys.size()) handle(outSlope, 1.0f);
                }
                const ImVec4 fill = selected ? selectedColour : curveColor(track, k);
                drawList->AddCircleFilled(ImVec2(x, y), keyRadius, color(fill, readOnly ? 0.6f : 1.0f));
                drawList->AddCircle(ImVec2(x, y), keyRadius, IM_COL32(20, 20, 20, 255), 0, 1.0f * m.s);
                points.push_back({t, keyIndex, (int)k, it->time, values[k]});
            }
        }
    }

    // ---- input: select, box, drag (time + value), menus, zoom and pan ----
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::InvisibleButton("##curve_graph", ImVec2(graphW, y1 - y0),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    // the key point under the mouse (the nearest)
    int hoveredPoint = -1;
    float best = m.keyHit * m.keyHit;
    for (size_t i = 0; i < points.size() && hovered; i++){
        const float dx = xAt(points[i].time) - io.MousePos.x;
        const float dy = yAt(points[i].value) - io.MousePos.y;
        if (dx * dx + dy * dy <= best){
            best = dx * dx + dy * dy;
            hoveredPoint = (int)i;
        }
    }
    auto pointId = [&](const CurvePoint& point){
        const ClipTrackData& track = data.tracks[point.track];
        return KeyId{track.path, track.channel, point.time};
    };
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        const bool additive = io.KeyShift || io.KeyCtrl;
        dragStart = io.MousePos;
        if (hoveredPoint >= 0){
            const CurvePoint& point = points[(size_t)hoveredPoint];
            const bool already = isSelected(point.track, point.key);
            if (io.KeyCtrl && already){
                std::vector<KeyId> kept;
                const KeyId id = pointId(point);
                for (const KeyId& other : selection){
                    if (!(other.path == id.path && other.channel == id.channel && std::fabs(other.time - id.time) <= kKeyEpsilon)) kept.push_back(other);
                }
                selection = kept;
                refreshSelection();
                drag = Drag::None;
            }else{
                if (!already) selectKeys({pointId(point)}, additive);
                dragGrabTime = point.time;
                curveGrabTrack = (int)point.track;
                curveGrabComponent = point.component;
                curveGrabValue = point.value;
                dragAdditive = additive;
                drag = Drag::PendingCurve;
            }
        }else{
            if (!additive){
                selection.clear();
                selectEvent(-1);
                refreshSelection();
            }
            dragAdditive = additive;
            drag = Drag::CurveBox;
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        const ImVec2 moved(io.MousePos.x - dragStart.x, io.MousePos.y - dragStart.y);
        if (drag == Drag::PendingCurve && (std::fabs(moved.x) > 3.0f * m.s || std::fabs(moved.y) > 3.0f * m.s)){
            std::string error;
            if (!readOnly && beginCurveDrag(curveGrabTrack, curveGrabComponent, &error)){
                if (curveValueLocked && std::fabs(moved.y) > std::fabs(moved.x)){
                    setNotice("Value drags move the keys of one property (or one rotation key) at a time: this drag moves time only", true);
                }
            }else{
                setNotice(readOnly ? std::string("Read-only clip: extract it to edit it") : error, true);
                drag = Drag::None;
            }
        }
        if (drag == Drag::CurveKeys){
            // Shift keeps the drag on its main axis
            float dx = moved.x, dy = moved.y;
            if (io.KeyShift){
                if (std::fabs(dx) >= std::fabs(dy)) dy = 0.0f;
                else dx = 0.0f;
            }
            std::string error;
            if (!updateCurveDrag(dx / pixelsPerSecond, -dy / pixelsPerValue, &error) && !error.empty()) setNotice(error, true);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (drag == Drag::CurveBox){
            const ImVec2 a(std::min(dragStart.x, io.MousePos.x), std::min(dragStart.y, io.MousePos.y));
            const ImVec2 b(std::max(dragStart.x, io.MousePos.x), std::max(dragStart.y, io.MousePos.y));
            drawList->AddRectFilled(a, b, color(Theme::Colors::Selection, 0.25f));
            drawList->AddRect(a, b, color(Theme::Colors::Selection, 0.9f));
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (drag == Drag::CurveKeys || drag == Drag::CurveBox || drag == Drag::PendingCurve)){
        if (drag == Drag::CurveKeys){
            endDrag();
        }else if (drag == Drag::CurveBox){
            const ImVec2 a(std::min(dragStart.x, io.MousePos.x), std::min(dragStart.y, io.MousePos.y));
            const ImVec2 b(std::max(dragStart.x, io.MousePos.x), std::max(dragStart.y, io.MousePos.y));
            if (b.x - a.x > 2.0f * m.s || b.y - a.y > 2.0f * m.s){
                std::vector<KeyId> found;
                for (const CurvePoint& point : points){
                    const float x = xAt(point.time), y = yAt(point.value);
                    if (x >= a.x && x <= b.x && y >= a.y && y <= b.y) found.push_back(pointId(point));
                }
                selectKeys(found, dragAdditive);
            }
            drag = Drag::None;
        }else{
            // a click (no drag) on one of several selected keys selects it alone
            if (!dragAdditive && hoveredPoint >= 0) selectKeys({pointId(points[(size_t)hoveredPoint])}, false);
            drag = Drag::None;
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && drag == Drag::None){
        contextRow = -1;
        contextTime = snapToFrame(std::max(0.0f, timeAt(io.MousePos.x)));
        if (hoveredPoint >= 0){
            const CurvePoint& point = points[(size_t)hoveredPoint];
            if (!isSelected(point.track, point.key)) selectKeys({pointId(point)}, false);
            contextRow = rowOfTrack(point.track);
            contextTime = point.time;
        }else if (clip){
            // a curve segment under the mouse: its ease is its first key's (the menu's Interpolation)
            const float time = timeAt(io.MousePos.x);
            for (size_t t = 0; t < data.tracks.size(); t++){
                const ClipTrackData& track = data.tracks[t];
                const int index = clip->findTrack(track.path, track.channel);
                if (index < 0 || track.keys.size() < 2 || time < track.keys.front().time || time > track.keys.back().time) continue;
                std::vector<std::vector<float>> sample;
                if (!clipcurves::sampleCurves(clip->getTracks()[(size_t)index], track, {time}, sample)) continue;
                bool near = false;
                for (size_t k = 0; k < sample[0].size(); k++){
                    near = near || (curveVisible(track, (int)k) && std::fabs(yAt(sample[0][k]) - io.MousePos.y) <= 5.0f * m.s);
                }
                if (!near) continue;
                const auto after = std::upper_bound(track.keys.begin(), track.keys.end(), time,
                                                    [](float value, const ClipKey& k){ return value < k.time; });
                const size_t segmentKey = (size_t)std::max<long>(0, (long)(after - track.keys.begin()) - 1);
                selectKeys({KeyId{track.path, track.channel, track.keys[segmentKey].time}}, false);
                contextRow = rowOfTrack(t);
                break;
            }
        }
        ImGui::OpenPopup("##clip_key_menu");
    }
    drawKeyContextMenu();
    if (hovered && hoveredPoint >= 0 && drag == Drag::None){
        const CurvePoint& point = points[(size_t)hoveredPoint];
        const ClipTrackData& track = data.tracks[point.track];
        const std::vector<std::string> names = track.channel == ClipChannel::Morph ? componentLabels(point.track) : std::vector<std::string>();
        ImGui::SetTooltip("%s %s\n%s\n%.4g%s", rows[(size_t)std::max(0, rowOfTrack(point.track))].leaf.c_str(),
                          clipcurves::componentLabel(track, (size_t)point.component, names).c_str(),
                          formatBoth(point.time, fps, showFrames).c_str(), point.value,
                          track.channel == ClipChannel::Rotation ? " deg" : "");
    }
    // the wheel zooms the values at the mouse (Ctrl: time, Shift: pan, as in the dopesheet); the
    // middle button pans both
    if (hovered && io.MouseWheel != 0.0f && !io.KeyCtrl && !io.KeyShift){
        const float anchor = valueAt(io.MousePos.y);
        const float factor = 1.0f - io.MouseWheel * 0.1f;
        const float low = anchor - (anchor - curveMin) * factor;
        const float high = anchor + (curveMax - anchor) * factor;
        if (high - low > 1e-5f && high - low < 1e7f){
            curveMin = low;
            curveMax = high;
        }
    }
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)){
        const float shift = io.MouseDelta.y / pixelsPerValue;
        curveMin += shift;
        curveMax += shift;
    }

    // the playhead
    const float playX = xAt(c.getTime());
    if (playX >= x0 && playX <= x1) drawList->AddLine(ImVec2(playX, y0), ImVec2(playX, y1), color(playheadColor()), 1.5f * m.s);
    drawList->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y1));
}
