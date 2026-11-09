// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimTimelineOps.h"

#include "TitleAnimOps.h"
#include "action/Ease.h"
#include "animation/AnimationClipIO.h"
#include "math/Curve.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleart/anim/TitleAnimIO.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;

namespace {

    constexpr double MERGE = 1e-4;

    TitleAnimJson setTrackOp(const ta::TitleAnimation& a, const ta::TitleTrack& t, const std::vector<ta::TitleKey>& keys){
        TitleAnimJson op;
        op["op"] = "set_track";
        op["animation"] = a.name;
        op["layer"] = titleLayerRef(t.layer);
        op["field"] = t.field;
        TitleAnimJson array = TitleAnimJson::array();
        for (const ta::TitleKey& k : keys) array.push_back(ta::encodeKey(k));
        op["keys"] = array;
        return op;
    }

    std::map<int, std::set<int>> byTrack(const ta::TitleAnimation& a, const std::vector<TitleKeyRef>& selection){
        std::map<int, std::set<int>> out;
        for (const TitleKeyRef& r : selection){
            if (r.track < 0 || r.track >= (int)a.tracks.size()) continue;
            if (r.key < 0 || r.key >= (int)a.tracks[(size_t)r.track].keys.size()) continue;
            out[r.track].insert(r.key);
        }
        return out;
    }

    // the unselected keys and the moved ones, sorted by time; a moved key replaces an unmoved one
    // at its time
    template<typename Move>
    std::vector<ta::TitleKey> reshape(const ta::TitleTrack& t, const std::set<int>& selected, Move move){
        std::vector<ta::TitleKey> moved, rest;
        for (int i = 0; i < (int)t.keys.size(); i++){
            ta::TitleKey k = t.keys[(size_t)i];
            if (selected.count(i)){
                k.key.time = (float)std::max(0.0, (double)move(k.key.time));
                moved.push_back(k);
            }else{
                rest.push_back(k);
            }
        }
        std::vector<ta::TitleKey> out;
        for (const ta::TitleKey& k : rest){
            const bool covered = std::any_of(moved.begin(), moved.end(), [&](const ta::TitleKey& m){ return std::fabs(m.key.time - k.key.time) <= MERGE; });
            if (!covered) out.push_back(k);
        }
        out.insert(out.end(), moved.begin(), moved.end());
        std::stable_sort(out.begin(), out.end(), [](const ta::TitleKey& x, const ta::TitleKey& y){ return x.key.time < y.key.time; });
        return out;
    }

    bool sameKeys(const std::vector<ta::TitleKey>& a, const std::vector<ta::TitleKey>& b){
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) if (a[i].key.time != b[i].key.time) return false;
        return true;
    }

}

double jasperengine::editor::snapTitleKeyTime(double t, int fps){
    if (fps <= 0) return t;
    return std::round(t * fps) / (double)fps;
}

std::vector<float> jasperengine::editor::titleEaseSamples(EaseType ease, int count){
    std::vector<float> out;
    const auto function = Ease::getFunction(ease);
    for (int i = 0; i < count; i++){
        const float t = count > 1 ? (float)i / (float)(count - 1) : 0.0f;
        out.push_back(function(t));
    }
    return out;
}

TitleAnimJson jasperengine::editor::titleMoveKeysOps(const ta::TitleAnimation& a, const std::vector<TitleKeyRef>& selection, double shift, int snapFps){
    TitleAnimJson ops = TitleAnimJson::array();
    for (const auto& [track, keys] : byTrack(a, selection)){
        const ta::TitleTrack& t = a.tracks[(size_t)track];
        const std::vector<ta::TitleKey> out = reshape(t, keys, [&](double time){ return snapTitleKeyTime(time + shift, snapFps); });
        if (!sameKeys(out, t.keys)) ops.push_back(setTrackOp(a, t, out));
    }
    return ops;
}

TitleAnimJson jasperengine::editor::titleScaleKeysOps(const ta::TitleAnimation& a, const std::vector<TitleKeyRef>& selection, double pivot, double factor, int snapFps){
    TitleAnimJson ops = TitleAnimJson::array();
    for (const auto& [track, keys] : byTrack(a, selection)){
        const ta::TitleTrack& t = a.tracks[(size_t)track];
        const std::vector<ta::TitleKey> out = reshape(t, keys, [&](double time){ return snapTitleKeyTime(pivot + (time - pivot) * factor, snapFps); });
        if (!sameKeys(out, t.keys)) ops.push_back(setTrackOp(a, t, out));
    }
    return ops;
}

TitleAnimJson jasperengine::editor::titleDeleteKeysOps(const ta::TitleAnimation& a, const std::vector<TitleKeyRef>& selection){
    TitleAnimJson ops = TitleAnimJson::array();
    for (const auto& [track, keys] : byTrack(a, selection)){
        const ta::TitleTrack& t = a.tracks[(size_t)track];
        if (keys.size() == t.keys.size()){
            // removing the last key removes the track (set_track refuses no keys)
            TitleAnimJson op;
            op["op"] = "remove_track";
            op["animation"] = a.name;
            op["layer"] = titleLayerRef(t.layer);
            op["field"] = t.field;
            ops.push_back(op);
            continue;
        }
        std::vector<ta::TitleKey> out;
        for (int i = 0; i < (int)t.keys.size(); i++) if (!keys.count(i)) out.push_back(t.keys[(size_t)i]);
        ops.push_back(setTrackOp(a, t, out));
    }
    return ops;
}

TitleAnimJson jasperengine::editor::titleDuplicateKeysOps(const ta::TitleAnimation& a, const std::vector<TitleKeyRef>& selection, double delta){
    TitleAnimJson ops = TitleAnimJson::array();
    for (const auto& [track, keys] : byTrack(a, selection)){
        const ta::TitleTrack& t = a.tracks[(size_t)track];
        std::vector<ta::TitleKey> out = t.keys;
        for (int i : keys){
            ta::TitleKey copy = t.keys[(size_t)i];
            copy.key.time = (float)(copy.key.time + delta);
            auto same = std::find_if(out.begin(), out.end(), [&](const ta::TitleKey& k){ return std::fabs(k.key.time - copy.key.time) <= MERGE; });
            if (same != out.end()) *same = copy;
            else out.push_back(copy);
        }
        std::stable_sort(out.begin(), out.end(), [](const ta::TitleKey& x, const ta::TitleKey& y){ return x.key.time < y.key.time; });
        ops.push_back(setTrackOp(a, t, out));
    }
    return ops;
}

std::vector<TitleKeyClipboard> jasperengine::editor::titleCopyKeys(const ta::TitleAnimation& a, const std::vector<TitleKeyRef>& selection){
    std::vector<TitleKeyClipboard> out;
    double first = 1e30;
    const auto tracks = byTrack(a, selection);
    for (const auto& [track, keys] : tracks) for (int i : keys) first = std::min(first, (double)a.tracks[(size_t)track].keys[(size_t)i].key.time);
    for (const auto& [track, keys] : tracks){
        TitleKeyClipboard clip;
        clip.layer = a.tracks[(size_t)track].layer;
        clip.field = a.tracks[(size_t)track].field;
        for (int i : keys){
            ta::TitleKey k = a.tracks[(size_t)track].keys[(size_t)i];
            k.key.time = (float)(k.key.time - first);
            clip.keys.push_back(k);
        }
        out.push_back(clip);
    }
    return out;
}

TitleAnimJson jasperengine::editor::titlePasteKeysOps(const ta::TitleAnimation& a, const std::vector<TitleKeyClipboard>& clipboard, double at){
    TitleAnimJson ops = TitleAnimJson::array();
    for (const TitleKeyClipboard& clip : clipboard){
        const int index = findTitleTrack(a, clip.layer, clip.field);
        if (index < 0) continue;
        const ta::TitleTrack& t = a.tracks[(size_t)index];
        std::vector<ta::TitleKey> out = t.keys;
        for (ta::TitleKey k : clip.keys){
            k.key.time = (float)(at + k.key.time);
            auto same = std::find_if(out.begin(), out.end(), [&](const ta::TitleKey& x){ return std::fabs(x.key.time - k.key.time) <= MERGE; });
            if (same != out.end()) *same = k;
            else out.push_back(k);
        }
        std::stable_sort(out.begin(), out.end(), [](const ta::TitleKey& x, const ta::TitleKey& y){ return x.key.time < y.key.time; });
        ops.push_back(setTrackOp(a, t, out));
    }
    return ops;
}

TitleAnimJson jasperengine::editor::titleSetEaseOps(const ta::TitleAnimation& a, const std::vector<TitleKeyRef>& selection, EaseType ease){
    TitleAnimJson ops = TitleAnimJson::array();
    for (const auto& [track, keys] : byTrack(a, selection)){
        const ta::TitleTrack& t = a.tracks[(size_t)track];
        std::vector<ta::TitleKey> out = t.keys;
        for (int i : keys){
            out[(size_t)i].key.ease = ease;
            // an ease replaces the Hermite segment (as the Animation window's Ease menu)
            out[(size_t)i].key.inTangent.clear();
            out[(size_t)i].key.outTangent.clear();
            out[(size_t)i].key.tangentMode = ClipTangentMode::None;
        }
        ops.push_back(setTrackOp(a, t, out));
    }
    return ops;
}

TitleAnimJson jasperengine::editor::titleSetTangentModeOps(const ta::TitleAnimation& a, const std::vector<TitleKeyRef>& selection, ClipTangentMode mode){
    TitleAnimJson ops = TitleAnimJson::array();
    for (const auto& [track, keys] : byTrack(a, selection)){
        const ta::TitleTrack& t = a.tracks[(size_t)track];
        std::vector<ta::TitleKey> out = t.keys;
        const int width = out.empty() ? 1 : (int)std::max<size_t>(1, out.front().key.value.size());
        for (int i : keys){
            ClipKey& k = out[(size_t)i].key;
            k.tangentMode = mode;
            k.ease = mode == ClipTangentMode::Constant ? EaseType::STEP : EaseType::LINEAR;
            // the op normalizes tangents from the mode; both sides present and of the value's width
            k.inTangent.assign((size_t)width, 0.0f);
            k.outTangent.assign((size_t)width, 0.0f);
        }
        ops.push_back(setTrackOp(a, t, out));
    }
    return ops;
}

TitleAnimJson jasperengine::editor::titleConvertEasesOps(const ta::TitleAnimation& a, int track){
    TitleAnimJson ops = TitleAnimJson::array();
    if (track < 0 || track >= (int)a.tracks.size()) return ops;
    const ta::TitleTrack& t = a.tracks[(size_t)track];
    std::vector<ta::TitleKey> out;
    bool changed = false;
    for (size_t i = 0; i < t.keys.size(); i++){
        const ta::TitleKey& k = t.keys[i];
        if (i + 1 >= t.keys.size() || k.key.ease == EaseType::LINEAR || k.key.ease == EaseType::STEP){
            out.push_back(k);
            continue;
        }
        const ta::TitleKey& next = t.keys[i + 1];
        const Curve curve = Curve::fromEase(k.key.ease);
        const float span = next.key.time - k.key.time;
        const size_t width = k.key.value.size();
        for (size_t c = 0; c < curve.keys.size(); c++){
            ta::TitleKey sample = k;
            const CurveKey& key = curve.keys[c];
            sample.key.time = k.key.time + key.time * span;
            sample.key.ease = EaseType::LINEAR;
            sample.key.tangentMode = ClipTangentMode::Free;
            sample.key.value.resize(width);
            sample.key.inTangent.resize(width);
            sample.key.outTangent.resize(width);
            for (size_t w = 0; w < width; w++){
                const float range = next.key.value[w] - k.key.value[w];
                sample.key.value[w] = k.key.value[w] + key.value * range;
                sample.key.inTangent[w] = key.inTangent * range / std::max(span, 1e-6f);
                sample.key.outTangent[w] = key.outTangent * range / std::max(span, 1e-6f);
            }
            if (c == 0 || c + 1 == curve.keys.size()) sample.key.value = c == 0 ? k.key.value : next.key.value;
            if (c + 1 == curve.keys.size()) continue;     // the next key starts there
            out.push_back(sample);
        }
        changed = true;
    }
    if (changed) ops.push_back(setTrackOp(a, t, out));
    return ops;
}

TitleRecordResult jasperengine::editor::titleRecordRewrite(const tt::TitleArtDoc& doc, const ta::TitleAnimation& a, double time, const TitleAnimJson& ops){
    TitleRecordResult out;
    if (!ops.is_array()){
        out.ops = ops;
        return out;
    }
    for (const TitleAnimJson& op : ops){
        if (!op.is_object() || op.value("op", std::string()) != "set" || !op.contains("values") || !op["values"].is_object()){
            out.ops.push_back(op);
            continue;
        }
        // the Designer names a layer "id:<id>"; tracks and fields hold the bare id
        const std::string layer = titleLayerIdOf(op.value("layer", std::string()));
        TitleAnimJson rest = op;
        rest["values"] = TitleAnimJson::object();
        for (auto it = op["values"].begin(); it != op["values"].end(); ++it){
            const tt::TrackFieldInfo info = tt::resolveTrackField(doc, layer, it.key());
            if (!info.ok || info.anim == tt::FxAnim::None){
                rest["values"][it.key()] = it.value();
                out.notKeyed.push_back(info.ok && !info.label.empty() ? info.label : it.key());
                continue;
            }
            // a new track starts from the still at 0 (R45)
            if (findTitleTrack(a, layer, info.longPath) < 0 && time > MERGE){
                TitleAnimJson zero;
                zero["op"] = "set_key";
                zero["animation"] = a.name;
                zero["layer"] = titleLayerRef(layer);
                zero["field"] = info.longPath;
                zero["time"] = 0.0;
                zero["value"] = sampleTitleTrackValue(doc, a, layer, info.longPath, 0.0);
                out.ops.push_back(zero);
            }
            TitleAnimJson key;
            key["op"] = "set_key";
            key["animation"] = a.name;
            key["layer"] = titleLayerRef(layer);
            key["field"] = info.longPath;
            key["time"] = time;
            key["value"] = it.value();
            out.ops.push_back(key);
            const std::string owner = layer.empty() ? std::string("Title") : info.layer ? info.layer->value("name", layer) : layer;
            out.stepName = "Record Key: " + owner + "/" + (info.label.empty() ? info.longPath : info.label);
        }
        if (!rest["values"].empty()) out.ops.push_back(rest);
    }
    return out;
}
