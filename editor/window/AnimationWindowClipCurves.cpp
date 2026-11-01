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
#include "animation/ClipTangents.h"
#include "animation/KeyframeSampling.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
            // a property's components (a swizzle track: its one); unknown properties none
            return track.known ? track.width : 0;
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
        case ClipChannel::Property: {
            static const char* const colors[4] = {"r", "g", "b", "a"};
            static const char* const vectors[4] = {"x", "y", "z", "w"};
            if (track.width <= 1 || component >= 4) return track.property;
            return (track.kind == PropertyKind::Color3 || track.kind == PropertyKind::Color4) ? colors[component] : vectors[component];
        }
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
    std::vector<float> flat;
    const std::vector<Vector3> eulers = track.channel == ClipChannel::Rotation ? keyEulers(track) : std::vector<Vector3>();
    if (!sampleCurvesFlat(runtime, track, eulers, times, flat)) return false;
    const size_t count = componentCount(track);
    out.reserve(times.size());
    for (size_t i = 0; i < times.size(); i++) out.emplace_back(flat.begin() + (long)(i * count), flat.begin() + (long)((i + 1) * count));
    return true;
}

bool clipcurves::sampleCurvesFlat(const AnimationClip::Track& runtime, const ClipTrackData& track, const std::vector<Vector3>& eulers,
                                  const std::vector<float>& times, std::vector<float>& out){
    out.clear();
    if (track.keys.empty() || !runtime.isSampled()) return false;
    const bool rotation = track.channel == ClipChannel::Rotation;
    if (rotation && eulers.size() != track.keys.size()) return false;
    const size_t count = componentCount(track);
    out.assign(times.size() * count, 0.0f);
    anim::KeyCursor cursor;
    anim::TrackSample sample;
    // rotation continuity: the segment of the previous sample and its Euler
    long previousSegment = -2;
    Vector3 previous(0, 0, 0);
    // the key at or before the time, walked forward (times ascend)
    size_t nextKey = 0;
    std::vector<float> xyzw(4);
    for (size_t i = 0; i < times.size(); i++){
        const float time = times[i];
        float* row = out.data() + i * count;
        if (!anim::sampleTrack(runtime, time, sample, &cursor) || !sample.valid) continue;
        switch (track.channel){
            case ClipChannel::Position:
            case ClipChannel::Scale:
                row[0] = sample.vector.x;
                row[1] = sample.vector.y;
                row[2] = sample.vector.z;
                break;
            case ClipChannel::Rotation: {
                while (nextKey < track.keys.size() && track.keys[nextKey].time <= time + kKeyEpsilon) nextKey++;
                const long segment = (long)nextKey - 1;
                const size_t keyIndex = (size_t)std::max(0L, segment);
                Vector3 euler;
                if (segment >= 0 && std::fabs(track.keys[keyIndex].time - time) <= kKeyEpsilon){
                    // at a key: the key's continuous Euler (the curve passes through its point)
                    euler = eulers[keyIndex];
                }else{
                    quaternionToXYZW(sample.rotation, xyzw.data());
                    const Vector3 reference = segment == previousSegment ? previous : eulers[keyIndex];
                    euler = nearestEuler(eulerOf(xyzw), reference);
                }
                previousSegment = segment;
                previous = euler;
                row[0] = euler.x;
                row[1] = euler.y;
                row[2] = euler.z;
                break;
            }
            case ClipChannel::Morph:
            case ClipChannel::Property:
                for (size_t c = 0; c < count && c < sample.weights.size(); c++) row[c] = sample.weights[c];
                break;
            case ClipChannel::Lattice:
                for (size_t p = 0; p < sample.offsets.size() && p * 3 + 2 < count; p++){
                    row[p * 3] = sample.offsets[p].x;
                    row[p * 3 + 1] = sample.offsets[p].y;
                    row[p * 3 + 2] = sample.offsets[p].z;
                }
                break;
        }
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

    // x red, y green, z blue; blend shapes cycle through the Theme's accents; a property's
    // r g b (x y z) the same and its a (w) light grey, one value the accent of its kind
    ImVec4 curveColor(const ClipTrackData& track, size_t component){
        if (track.channel == ClipChannel::Property){
            if (track.width <= 1) return track.discrete ? Theme::Colors::WarningText : Theme::Colors::ChildSceneText;
            if (component == 3) return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        }
        if (track.channel == ClipChannel::Morph){
            const ImVec4 palette[] = {Theme::Colors::WarningText, Theme::Colors::ChildSceneText, Theme::Colors::SubSelectionText,
                                      Theme::Colors::PrefabText, Theme::Colors::PhysicsAccent, Theme::Colors::ErrorText,
                                      ImVec4(0.35f, 0.85f, 0.35f, 1.0f), Theme::Colors::LogIcon};
            return palette[component % (sizeof(palette) / sizeof(palette[0]))];
        }
        switch (component % 3){
            case 0: return Theme::Colors::ErrorText;
            case 1: return ImVec4(0.35f, 0.85f, 0.35f, 1.0f);   // a saturated green, apart from the warning yellow
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

    // a track's content (keys, times, eases, tangents, points): its samples are reused while equal
    uint64_t trackHash(const ClipTrackData& track){
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v){ h = (h ^ v) * 1099511628211ull; };
        auto bits = [](float f){ uint32_t u; std::memcpy(&u, &f, sizeof(u)); return (uint64_t)u; };
        mix((uint64_t)track.channel);
        mix(track.keys.size());
        mix(track.loop ? 1 : 0);
        for (int point : track.points) mix((uint64_t)(uint32_t)point);
        for (const ClipKey& key : track.keys){
            mix(bits(key.time));
            mix((uint64_t)key.ease);
            for (float v : key.value) mix(bits(v));
            for (float v : key.inTangent) mix(bits(v));
            for (float v : key.outTangent) mix(bits(v));
        }
        return h;
    }

}

bool AnimationWindowClip::curveVisible(const ClipTrackData& track, int component) const{
    return !hiddenCurves.count(curveKey(track, component));
}

void AnimationWindowClip::setCurveVisible(const ClipTrackData& track, int component, bool visible){
    const size_t before = hiddenCurves.size();
    if (visible) hiddenCurves.erase(curveKey(track, component));
    else hiddenCurves.insert(curveKey(track, component));
    if (hiddenCurves.size() != before) hiddenVersion++;
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

void AnimationWindowClip::refreshCurveCache(float from, float to, float stepTime){
    std::shared_ptr<AnimationClipDocument> doc = document();
    CurveCache& cache = curveCache;
    if (!doc){
        cache = CurveCache();
        return;
    }
    const bool sameDocument = doc == cache.document.lock();
    const bool sameRange = std::fabs(from - cache.from) < 1e-6f && std::fabs(to - cache.to) < 1e-6f &&
                           std::fabs(pixelsPerSecond - cache.pixelsPerSecond) < 1e-6f;
    if (sameDocument && sameRange && doc->revision() == cache.revision && hiddenVersion == cache.hiddenVersion) return;
    const ClipData& data = doc->data();
    std::shared_ptr<const AnimationClip> clip = doc->clip();
    const bool keepSamples = sameDocument && sameRange;
    cache.document = doc;
    cache.from = from;
    cache.to = to;
    cache.pixelsPerSecond = pixelsPerSecond;
    cache.revision = doc->revision();
    cache.hiddenVersion = hiddenVersion;
    if (cache.tracks.size() != data.tracks.size()) cache.tracks.assign(data.tracks.size(), CurveTrackCache());
    // the sample grid (every 2 px) of the visible range
    std::vector<float> grid;
    for (double t = from; t <= (double)to + stepTime && grid.size() < 4096; t += stepTime) grid.push_back((float)t);
    for (size_t t = 0; t < data.tracks.size(); t++){
        const ClipTrackData& track = data.tracks[t];
        CurveTrackCache& entry = cache.tracks[t];
        const uint64_t hash = trackHash(track);
        const size_t count = clipcurves::componentCount(track);
        const bool contentSame = entry.contentHash == hash && entry.components == count;
        // visibility (no strings per frame afterwards)
        entry.visible.assign(count, 1);
        entry.anyVisible = false;
        for (size_t c = 0; c < count; c++){
            entry.visible[c] = curveVisible(track, (int)c) ? 1 : 0;
            entry.anyVisible = entry.anyVisible || entry.visible[c];
        }
        if (contentSame && keepSamples && (!entry.times.empty() || !entry.anyVisible)) continue;
        if (!contentSame){
            entry.contentHash = hash;
            entry.components = count;
            entry.eulers = track.channel == ClipChannel::Rotation ? clipcurves::keyEulers(track) : std::vector<Vector3>();
            entry.keyValues.assign(track.keys.size() * count, 0.0f);
            for (size_t k = 0; k < track.keys.size(); k++){
                if (track.channel == ClipChannel::Rotation){
                    entry.keyValues[k * count] = entry.eulers[k].x;
                    entry.keyValues[k * count + 1] = entry.eulers[k].y;
                    entry.keyValues[k * count + 2] = entry.eulers[k].z;
                }else{
                    for (size_t c = 0; c < count && c < track.keys[k].value.size(); c++) entry.keyValues[k * count + c] = track.keys[k].value[c];
                }
            }
        }
        entry.times.clear();
        entry.values.clear();
        const int index = clip ? builtTrack(*clip, track) : -1;
        if (!entry.anyVisible || index < 0 || track.keys.empty()) continue;
        // the grid, plus the keys' own times while keys are sparser than the grid (dense keys:
        // the 2 px grid already follows the curve)
        entry.times = grid;
        const auto first = std::lower_bound(track.keys.begin(), track.keys.end(), from,
                                             [](const ClipKey& k, float time){ return k.time < time; });
        const auto last = std::upper_bound(track.keys.begin(), track.keys.end(), to,
                                           [](float time, const ClipKey& k){ return time < k.time; });
        if ((size_t)(last - first) * 2 < grid.size() + 1){
            for (auto it = first; it != last; ++it) entry.times.push_back(it->time);
            std::sort(entry.times.begin(), entry.times.end());
        }
        clipcurves::sampleCurvesFlat(clip->getTracks()[(size_t)index], track, entry.eulers, entry.times, entry.values);
    }
}

void AnimationWindowClip::refreshCurveSelection(){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        curveSelected.clear();
        curveSelectedKeys.clear();
        return;
    }
    if (doc == curveSelectedDocument.lock() && doc->revision() == curveSelectedRevision && curveSelectionVersion == curveSelectedVersion) return;
    curveSelectedDocument = doc;
    curveSelectedRevision = doc->revision();
    curveSelectedVersion = curveSelectionVersion;
    curveSelected.clear();
    curveSelectedKeys.clear();
    const ClipData& data = doc->data();
    for (const CurveKeyId& point : curveSelection){
        const int track = findTrackById(data, point.key.path, point.key.channel, point.key.property);
        if (track < 0) continue;
        const std::vector<ClipKey>& keys = data.tracks[(size_t)track].keys;
        auto it = std::lower_bound(keys.begin(), keys.end(), point.key.time - kKeyEpsilon,
                                   [](const ClipKey& k, float time){ return k.time < time; });
        if (it == keys.end() || std::fabs(it->time - point.key.time) > kKeyEpsilon) continue;
        const size_t key = (size_t)(it - keys.begin());
        curveSelected.insert({(size_t)track, key, point.component});
        curveSelectedKeys.insert({(size_t)track, key});
    }
}

bool AnimationWindowClip::curvePointSelected(size_t track, size_t key, int component) const{
    if (!isSelected(track, key)) return false;
    if (!curveSelectedKeys.count({track, key})) return true;
    return curveSelected.count({track, key, component}) > 0;
}

void AnimationWindowClip::selectCurvePoints(const std::vector<CurveKeyId>& points, bool additive){
    if (!additive){
        selection.clear();
        curveSelection.clear();
    }
    for (const CurveKeyId& point : points){
        bool listed = false;
        for (const KeyId& id : selection) listed = listed || id == point.key;
        if (!listed) selection.push_back(point.key);
        bool pointListed = false;
        for (const CurveKeyId& other : curveSelection){
            pointListed = pointListed || (other.key == point.key && other.component == point.component);
        }
        if (!pointListed) curveSelection.push_back(point);
    }
    curveSelectionVersion++;
    // one selection (Unity): keys and the event don't stay selected together
    selectEvent(-1);
    refreshSelection();
    refreshCurveSelection();
}

std::vector<int> AnimationWindowClip::curveDragComponentsOf(size_t trackIndex, size_t keyIndex) const{
    std::vector<int> out;
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || trackIndex >= doc->data().tracks.size()) return out;
    const ClipTrackData& track = doc->data().tracks[trackIndex];
    const size_t count = clipcurves::componentCount(track);
    const bool explicitPoints = curveSelectedKeys.count({trackIndex, keyIndex}) > 0;
    for (size_t c = 0; c < count; c++){
        if (explicitPoints ? curveSelected.count({trackIndex, keyIndex, (int)c}) > 0 : curveVisible(track, (int)c)) out.push_back((int)c);
    }
    return out;
}

void AnimationWindowClip::frameCurveValues(bool selectedOnly){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    refreshSelection();
    refreshCurveSelection();
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
            for (int c : curveDragComponentsOf(ref.track, ref.key)){
                if ((size_t)c < values.size()) take(values[(size_t)c]);
            }
        }
    }else{
        // every visible curve over the clip (overshooting eases included)
        std::shared_ptr<const AnimationClip> clip = doc->clip();
        const float length = std::max(data.effectiveLength(), 1e-3f);
        std::vector<float> times;
        for (int i = 0; i <= 256; i++) times.push_back(length * (float)i / 256.0f);
        std::vector<float> flat;
        for (size_t t = 0; t < data.tracks.size() && clip; t++){
            const ClipTrackData& track = data.tracks[t];
            const int index = builtTrack(*clip, track);
            if (index < 0) continue;
            const size_t count = clipcurves::componentCount(track);
            std::vector<char> visible(count);
            bool any = false;
            for (size_t c = 0; c < count; c++){
                visible[c] = curveVisible(track, (int)c) ? 1 : 0;
                any = any || visible[c];
            }
            if (!any) continue;
            std::vector<float> all = times;
            for (const ClipKey& key : track.keys) all.push_back(key.time);
            std::sort(all.begin(), all.end());
            if (!clipcurves::sampleCurvesFlat(clip->getTracks()[(size_t)index], track, trackEulers(t), all, flat)) continue;
            for (size_t i = 0; i < all.size(); i++){
                for (size_t c = 0; c < count; c++){
                    if (visible[c]) take(flat[i * count + c]);
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
    refreshCurveSelection();
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
    // per dragged key: the components its value moves and, for a rotation, its base Euler
    curveDragRefs = selectedRefs;
    curveDragComponents.clear();
    curveDragEulers.clear();
    for (const AnimationClipController::KeyRef& ref : curveDragRefs){
        curveDragComponents.push_back(curveDragComponentsOf(ref.track, ref.key));
        const std::vector<Vector3>& eulers = trackEulers(ref.track);
        curveDragEulers.push_back(ref.key < eulers.size() ? eulers[ref.key] : Vector3(0, 0, 0));
    }
    curveDragSelection = curveSelection;
    if (!controller().beginKeyDrag(curveDragRefs, error)) return false;
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
    const float value = std::isfinite(valueDelta) ? valueDelta : 0.0f;
    if (std::fabs(delta - curveDragTime) < 1e-6f && std::fabs(value - curveDragValue) < 1e-7f) return true;
    // every selected point moves by the value delta on its own curve: one absolute value per
    // dragged key from the drag's base (rotations through their continuous Euler)
    std::vector<AnimationClipController::KeyDragValue> values;
    if (value != 0.0f){
        const ClipData& base = controller().dragBaseData();
        for (size_t i = 0; i < curveDragRefs.size(); i++){
            const AnimationClipController::KeyRef& ref = curveDragRefs[i];
            if (curveDragComponents[i].empty() || ref.track >= base.tracks.size() || ref.key >= base.tracks[ref.track].keys.size()) continue;
            const ClipTrackData& track = base.tracks[ref.track];
            // stepped properties move in time only (their values are bools, ints, frames)
            if (track.channel == ClipChannel::Property && track.discrete) continue;
            AnimationClipController::KeyDragValue entry;
            entry.dragIndex = i;
            if (track.channel == ClipChannel::Rotation){
                Vector3 euler = curveDragEulers[i];
                for (int c : curveDragComponents[i]){
                    if (c == 0) euler.x += value;
                    if (c == 1) euler.y += value;
                    if (c == 2) euler.z += value;
                }
                entry.value = xyzwOf(euler);
            }else{
                entry.value = track.keys[ref.key].value;
                for (int c : curveDragComponents[i]){
                    if ((size_t)c < entry.value.size()) entry.value[(size_t)c] += value;
                }
            }
            values.push_back(entry);
        }
    }
    std::vector<AnimationClipController::KeyRef> moved;
    if (!controller().updateKeyDrag(delta, values, &moved, error)){
        controller().endKeyDrag();
        drag = Drag::None;
        return false;
    }
    curveDragTime = delta;
    curveDragValue = value;
    setSelectionFromRefs(moved);
    // the selected points follow their keys in time
    curveSelection = curveDragSelection;
    for (CurveKeyId& point : curveSelection) point.key.time = std::max(0.0f, point.key.time + delta);
    curveSelectionVersion++;
    refreshCurveSelection();
    return true;
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
    refreshCurveSelection();
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
            // the property's keys (as a click on a dopesheet label), its curves framed
            std::vector<KeyId> ids;
            for (const ClipKey& key : track.keys) ids.push_back(keyIdOf(track, key.time));
            selectKeys(ids, io.KeyShift || io.KeyCtrl);
            frameCurveValues(true);
        }
        if (row.missing) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s%s%s\nClick: select its keys and frame its curves", row.label.c_str(), row.message.empty() ? "" : "\n", row.message.c_str());
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
    // a neutral graph (the curves keep their contrast); Record and Preview show as a frame
    // (red / blue) and on the ruler
    drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color(ImVec4(0.15f, 0.15f, 0.15f, 1.0f)));
    const float clipEnd = xAt(data.effectiveLength());
    if (clipEnd < x1) drawList->AddRectFilled(ImVec2(std::max(x0, clipEnd), y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 60));
    const float visibleFrom = std::max(0.0f, timeAt(x0));
    const float visibleTo = std::max(visibleFrom, timeAt(x1));
    if (pixelsPerSecond / fps >= 8.0f * m.s){
        for (long f = (long)std::floor(visibleFrom * fps); f <= (long)std::ceil(visibleTo * fps); f++){
            const float x = xAt((float)f / fps);
            drawList->AddLine(ImVec2(x, y0), ImVec2(x, y1), IM_COL32(255, 255, 255, 12));
        }
    }

    // the samples of the visible range (cached until the clip, the zoom, the scroll or the
    // visibility change)
    refreshCurveCache(visibleFrom, visibleTo, 2.0f * m.s / pixelsPerSecond);

    // the value axis: nice ticks, grid lines, labels at the left edge; the unit while only
    // rotations (Euler degrees) are shown
    bool onlyRotations = true, anyShown = false;
    for (size_t t = 0; t < data.tracks.size() && t < curveCache.tracks.size(); t++){
        if (!curveCache.tracks[t].anyVisible) continue;
        anyShown = true;
        onlyRotations = onlyRotations && data.tracks[t].channel == ClipChannel::Rotation;
    }
    const float step = clipcurves::niceStep(curveMax - curveMin, graphH, 26.0f * m.s);
    for (float tick : clipcurves::ticks(curveMin, curveMax, step)){
        const float y = yAt(tick);
        drawList->AddLine(ImVec2(x0, y), ImVec2(x1, y), tick == 0.0f ? IM_COL32(255, 255, 255, 45) : IM_COL32(255, 255, 255, 18));
        const std::string label = formatValue(tick, step) + (anyShown && onlyRotations ? "\xC2\xB0" : "");
        drawList->AddText(ImVec2(x0 + 3.0f * m.s, y - ImGui::GetTextLineHeight() - 1.0f * m.s), color(Theme::Colors::SubtleText), label.c_str());
    }

    // the curves and key points from the cache
    std::vector<CurvePoint> points;
    curveHandles.clear();
    const float keyRadius = 4.0f * m.s;
    const ImVec4 selectedColour = selectedKeyColor();
    std::vector<ImVec2> line;
    const float bucket = 4.0f * m.s;       // dense keys: one point per 4 px column per curve
    for (size_t t = 0; t < data.tracks.size() && t < curveCache.tracks.size(); t++){
        const ClipTrackData& track = data.tracks[t];
        const CurveTrackCache& entry = curveCache.tracks[t];
        if (!entry.anyVisible || entry.times.empty() || entry.values.size() != entry.times.size() * entry.components) continue;
        const size_t count = entry.components;
        line.resize(entry.times.size());
        for (size_t k = 0; k < count; k++){
            if (!entry.visible[k]) continue;
            for (size_t i = 0; i < entry.times.size(); i++) line[i] = ImVec2(xAt(entry.times[i]), yAt(entry.values[i * count + k]));
            drawList->AddPolyline(line.data(), (int)line.size(), color(curveColor(track, k), readOnly ? 0.6f : 0.95f), ImDrawFlags_None, 1.0f * m.s);
        }
        // key points in the visible range: thinned where keys are denser than a few pixels
        // (selected points always show), outlined only where they are apart
        const auto first = std::lower_bound(track.keys.begin(), track.keys.end(), visibleFrom - m.keyHit / pixelsPerSecond,
                                             [](const ClipKey& key, float time){ return key.time < time; });
        const auto last = std::upper_bound(track.keys.begin(), track.keys.end(), visibleTo + m.keyHit / pixelsPerSecond,
                                           [](float time, const ClipKey& key){ return time < key.time; });
        const size_t begin = (size_t)(first - track.keys.begin()), end = (size_t)(last - track.keys.begin());
        if (begin >= end || entry.keyValues.size() != track.keys.size() * count) continue;
        const float spacing = end - begin > 1 ? (xAt(track.keys[end - 1].time) - xAt(track.keys[begin].time)) / (float)(end - begin - 1) : 1e9f;
        const bool outline = spacing >= 8.0f * m.s;
        for (size_t k = 0; k < count; k++){
            if (!entry.visible[k]) continue;
            const ImVec4 colour = curveColor(track, k);
            float lastX = -1e9f;
            for (size_t key = begin; key < end; key++){
                const float x = xAt(track.keys[key].time);
                const bool selected = curvePointSelected(t, key, (int)k);
                if (!selected && x - lastX < bucket) continue;
                lastX = x;
                const float value = entry.keyValues[key * count + k];
                const float y = yAt(value);
                if (outline || selected){
                    drawList->AddCircleFilled(ImVec2(x, y), keyRadius, color(selected ? selectedColour : colour, readOnly ? 0.6f : 1.0f), 10);
                    drawList->AddCircle(ImVec2(x, y), keyRadius, IM_COL32(20, 20, 20, 255), 10, 1.0f * m.s);
                }else{
                    const float r = 2.5f * m.s;
                    drawList->AddRectFilled(ImVec2(x - r, y - r), ImVec2(x + r, y + r), color(colour, readOnly ? 0.6f : 1.0f));
                }
                points.push_back({t, key, (int)k, track.keys[key].time, value});
                // the tangents of selected keys: editable handles on scalar curves (anim-properties:
                // Unity's tangent editing; a key without tangents shows its linear slopes), a
                // rotation's read-only from its sampled curve
                const bool editableTangents = !readOnly && cliptangents::editable(track);
                if (!selected || (!track.keys[key].hasTangents() && !editableTangents)) continue;
                float inSlope = 0.0f, outSlope = 0.0f;
                if (!track.keys[key].hasTangents()){
                    const ClipKey& keyed = track.keys[key];
                    if (key > 0 && k < keyed.value.size() && k < track.keys[key - 1].value.size()){
                        const float dt = keyed.time - track.keys[key - 1].time;
                        if (dt > 0.0f) inSlope = (keyed.value[k] - track.keys[key - 1].value[k]) / dt;
                    }
                    if (key + 1 < track.keys.size() && k < keyed.value.size() && k < track.keys[key + 1].value.size()){
                        const float dt = track.keys[key + 1].time - keyed.time;
                        if (dt > 0.0f) outSlope = (track.keys[key + 1].value[k] - keyed.value[k]) / dt;
                    }
                    if (key == 0) inSlope = outSlope;
                    if (key + 1 >= track.keys.size()) outSlope = inSlope;
                }else if (track.channel == ClipChannel::Rotation){
                    std::shared_ptr<const AnimationClip> clip = doc->clip();
                    const int index = clip ? builtTrack(*clip, track) : -1;
                    const float h = 0.25f / fps;
                    std::vector<float> around;
                    if (index < 0 || !clipcurves::sampleCurvesFlat(clip->getTracks()[(size_t)index], track, entry.eulers,
                                                                   {track.keys[key].time - h, track.keys[key].time, track.keys[key].time + h}, around)) continue;
                    inSlope = (around[count + k] - around[k]) / h;
                    outSlope = (around[2 * count + k] - around[count + k]) / h;
                }else{
                    const ClipKey& keyed = track.keys[key];
                    if (k >= keyed.inTangent.size() || k >= keyed.outTangent.size()) continue;
                    inSlope = keyed.inTangent[k];
                    outSlope = keyed.outTangent[k];
                }
                const float length = 28.0f * m.s;
                auto handle = [&](float slope, float side){
                    const ImVec2 dir(side, -slope * pixelsPerValue / pixelsPerSecond * side);
                    const float norm = std::sqrt(dir.x * dir.x + dir.y * dir.y);
                    const ImVec2 tip(x + dir.x / norm * length, y + dir.y / norm * length);
                    const bool dragged = drag == Drag::CurveHandle && draggedHandle.track == t && draggedHandle.key == key &&
                                         draggedHandle.component == (int)k && draggedHandle.side == (int)side;
                    const ImVec4 handleColour = editableTangents ? (dragged ? selectedColour : ImVec4(0.9f, 0.9f, 0.9f, 1.0f)) : Theme::Colors::SubtleText;
                    drawList->AddLine(ImVec2(x, y), tip, color(handleColour, 0.9f), 1.0f * m.s);
                    if (editableTangents){
                        drawList->AddCircleFilled(tip, 3.0f * m.s, color(handleColour, 0.95f));
                        curveHandles.push_back({t, key, (int)k, (int)side, ImVec2(x, y), tip});
                    }else{
                        drawList->AddCircle(tip, 2.5f * m.s, color(handleColour, 0.9f));
                    }
                };
                if (key > 0) handle(inSlope, -1.0f);
                if (key + 1 < track.keys.size()) handle(outSlope, 1.0f);
            }
        }
    }

    // ---- input: select, box, drag (time + value), menus, zoom and pan ----
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::InvisibleButton("##curve_graph", ImVec2(graphW, y1 - y0),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
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
        return CurveKeyId{keyIdOf(track, point.time), point.component};
    };
    // a tangent handle under the mouse (before the key points: a handle tip lies off its key)
    int hoveredHandle = -1;
    float bestHandle = m.keyHit * m.keyHit;
    for (size_t i = 0; i < curveHandles.size() && hovered; i++){
        const float dx = curveHandles[i].tip.x - io.MousePos.x;
        const float dy = curveHandles[i].tip.y - io.MousePos.y;
        if (dx * dx + dy * dy <= bestHandle){
            bestHandle = dx * dx + dy * dy;
            hoveredHandle = (int)i;
        }
    }
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoveredHandle >= 0){
        // a handle drag: that side's slope, one undo step per drag
        draggedHandle = curveHandles[(size_t)hoveredHandle];
        handleMergeKey = "tangent#" + std::to_string(++handleSerial);
        drag = Drag::CurveHandle;
    }else if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        const bool additive = io.KeyShift || io.KeyCtrl;
        dragStart = io.MousePos;
        if (hoveredPoint >= 0){
            const CurvePoint& point = points[(size_t)hoveredPoint];
            const bool already = curvePointSelected(point.track, point.key, point.component);
            if (io.KeyCtrl && already){
                // Ctrl toggles the point off
                const CurveKeyId id = pointId(point);
                std::vector<CurveKeyId> kept;
                if (!curveSelectedKeys.count({point.track, point.key})){
                    // a whole key: its other visible points stay
                    for (int comp : curveDragComponentsOf(point.track, point.key)){
                        if (comp != point.component) kept.push_back({id.key, comp});
                    }
                }
                for (const CurveKeyId& other : curveSelection){
                    if (!(other.key == id.key && other.component == id.component)) kept.push_back(other);
                }
                std::vector<KeyId> keys;
                for (const KeyId& key : selection){
                    if (!(key == id.key)) keys.push_back(key);
                }
                selection = keys;
                curveSelection.clear();
                selectCurvePoints(kept, true);
                drag = Drag::None;
            }else{
                if (!already) selectCurvePoints({pointId(point)}, additive);
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
                curveSelection.clear();
                curveSelectionVersion++;
                selectEvent(-1);
                refreshSelection();
            }
            dragAdditive = additive;
            drag = Drag::CurveBox;
        }
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && drag == Drag::CurveHandle){
        // the slope from the key point to the mouse, on the handle's side
        float dx = io.MousePos.x - draggedHandle.origin.x;
        const float dy = io.MousePos.y - draggedHandle.origin.y;
        const float minimum = 2.0f * m.s;
        if (draggedHandle.side < 0) dx = std::min(dx, -minimum);
        else dx = std::max(dx, minimum);
        const float slope = -(dy / pixelsPerValue) / (dx / pixelsPerSecond);
        std::string error;
        const cliptangents::Side side = draggedHandle.side < 0 ? cliptangents::Side::Left : cliptangents::Side::Right;
        if (!c.setTangentSlope({draggedHandle.track, draggedHandle.key}, (size_t)draggedHandle.component, side, slope, handleMergeKey, &error)){
            setNotice(error, true);
            drag = Drag::None;
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }
    if (drag == Drag::CurveHandle && ImGui::IsMouseReleased(ImGuiMouseButton_Left)){
        drag = Drag::None;
        handleMergeKey.clear();
    }
    if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        const ImVec2 moved(io.MousePos.x - dragStart.x, io.MousePos.y - dragStart.y);
        if (drag == Drag::PendingCurve && (std::fabs(moved.x) > 3.0f * m.s || std::fabs(moved.y) > 3.0f * m.s)){
            std::string error;
            if (readOnly || !beginCurveDrag(curveGrabTrack, curveGrabComponent, &error)){
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
                // every key point inside (also the thinned ones): from the cached key values
                std::vector<CurveKeyId> found;
                for (size_t t = 0; t < data.tracks.size() && t < curveCache.tracks.size(); t++){
                    const ClipTrackData& track = data.tracks[t];
                    const CurveTrackCache& entry = curveCache.tracks[t];
                    if (!entry.anyVisible || entry.keyValues.size() != track.keys.size() * entry.components) continue;
                    const auto from = std::lower_bound(track.keys.begin(), track.keys.end(), timeAt(a.x),
                                                       [](const ClipKey& key, float time){ return key.time < time; });
                    for (auto it = from; it != track.keys.end() && xAt(it->time) <= b.x; ++it){
                        const size_t key = (size_t)(it - track.keys.begin());
                        for (size_t k = 0; k < entry.components; k++){
                            if (!entry.visible[k]) continue;
                            const float y = yAt(entry.keyValues[key * entry.components + k]);
                            if (y >= a.y && y <= b.y) found.push_back({keyIdOf(track, it->time), (int)k});
                        }
                    }
                }
                selectCurvePoints(found, dragAdditive);
            }
            drag = Drag::None;
        }else{
            // a click (no drag) on one of several selected points selects it alone
            if (!dragAdditive && hoveredPoint >= 0) selectCurvePoints({pointId(points[(size_t)hoveredPoint])}, false);
            drag = Drag::None;
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && drag == Drag::None){
        contextRow = -1;
        contextTime = snapToFrame(std::max(0.0f, timeAt(io.MousePos.x)));
        std::shared_ptr<const AnimationClip> clip = doc->clip();
        if (hoveredPoint >= 0){
            const CurvePoint& point = points[(size_t)hoveredPoint];
            if (!curvePointSelected(point.track, point.key, point.component)) selectCurvePoints({pointId(point)}, false);
            contextRow = rowOfTrack(point.track);
            contextTime = point.time;
        }else if (clip){
            // a curve segment under the mouse: its ease is its first key's (the menu's Interpolation)
            const float time = timeAt(io.MousePos.x);
            std::vector<float> sample;
            for (size_t t = 0; t < data.tracks.size() && t < curveCache.tracks.size(); t++){
                const ClipTrackData& track = data.tracks[t];
                const CurveTrackCache& entry = curveCache.tracks[t];
                const int index = builtTrack(*clip, track);
                if (!entry.anyVisible || index < 0 || track.keys.size() < 2 || time < track.keys.front().time || time > track.keys.back().time) continue;
                if (!clipcurves::sampleCurvesFlat(clip->getTracks()[(size_t)index], track, entry.eulers, {time}, sample)) continue;
                bool near = false;
                for (size_t k = 0; k < entry.components; k++){
                    near = near || (entry.visible[k] && std::fabs(yAt(sample[k]) - io.MousePos.y) <= 5.0f * m.s);
                }
                if (!near) continue;
                const auto after = std::upper_bound(track.keys.begin(), track.keys.end(), time,
                                                    [](float value, const ClipKey& k){ return value < k.time; });
                const size_t segmentKey = (size_t)std::max<long>(0, (long)(after - track.keys.begin()) - 1);
                selectKeys({keyIdOf(track, track.keys[segmentKey].time)}, false);
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
    // middle button pans time and values
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
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)){
        const float shift = io.MouseDelta.y / pixelsPerValue;
        curveMin += shift;
        curveMax += shift;
        scrollX = std::max(0.0f, scrollX - io.MouseDelta.x);
    }

    // the playhead, and the Record (red) / Preview (blue) frame
    const float playX = xAt(c.getTime());
    if (playX >= x0 && playX <= x1) drawList->AddLine(ImVec2(playX, y0), ImVec2(playX, y1), color(playheadColor()), 1.5f * m.s);
    if (c.isRecording() || c.isPreviewing()){
        const ImVec4 frame = c.isRecording() ? Theme::Colors::ErrorText : Theme::Colors::Selection;
        drawList->AddRect(ImVec2(x0 + 1.0f * m.s, y0 + 1.0f * m.s), ImVec2(x1 - 1.0f * m.s, y1 - 1.0f * m.s), color(frame, 0.95f), 0.0f, 0, 2.0f * m.s);
    }
    drawList->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y1));
}
