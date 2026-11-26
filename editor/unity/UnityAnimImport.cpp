// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UnityAnimImport.h"
#include "UnityYaml.h"

#include "animation/AnimationClipIO.h"
#include "animation/AnimationEvent.h"
#include "animation/AnimatorAssetIO.h"
#include "animation/PropertyBindings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor::unity;
using Json = nlohmann::json;

namespace {

    constexpr double KEY_EPSILON = 1e-6;
    constexpr int CLASS_ANIMATOR = 95;
    constexpr int CLASS_MESHRENDERER = 23;
    constexpr int CLASS_SKINNEDMESHRENDERER = 137;
    constexpr int CLASS_SPRITERENDERER = 212;
    constexpr int CLASS_LIGHT = 108;
    constexpr int CLASS_CAMERA = 20;
    constexpr int CLASS_AUDIOSOURCE = 82;

    // one key of one Unity curve component (slopes in value units per second; Infinity: stepped)
    struct UKey {
        double t = 0.0;
        double v = 0.0;
        double in = 0.0;
        double out = 0.0;
        int mode = 0;           // Unity's tangentMode bits
        int weighted = 0;       // Unity's weightedMode (0: none)
    };

    struct UCurve {
        std::vector<UKey> keys;
        bool empty() const { return keys.empty(); }
    };

    bool stepped(double slope) { return !std::isfinite(slope); }

    // the Hermite segment [a, b] at t: value and slope (a stepped segment holds a's value)
    void evalSegment(const UKey& a, const UKey& b, double t, double& v, double& slope) {
        const double dt = b.t - a.t;
        if (dt <= 0.0 || stepped(a.out) || stepped(b.in)) {
            v = a.v;
            slope = 0.0;
            return;
        }
        const double s = (t - a.t) / dt;
        const double s2 = s * s, s3 = s2 * s;
        v = (2 * s3 - 3 * s2 + 1) * a.v + (s3 - 2 * s2 + s) * dt * a.out + (-2 * s3 + 3 * s2) * b.v + (s3 - s2) * dt * b.in;
        slope = ((6 * s2 - 6 * s) * a.v + (3 * s2 - 4 * s + 1) * dt * a.out + (-6 * s2 + 6 * s) * b.v + (3 * s2 - 2 * s) * dt * b.in) / dt;
    }

    // the curve at t from the left (the segment ending at t) or the right; `isStepped`: that
    // segment holds its first value
    void evalSided(const UCurve& c, double t, bool left, double& v, double& slope, bool* isStepped = nullptr) {
        if (isStepped) *isStepped = false;
        const std::vector<UKey>& k = c.keys;
        if (k.empty()) {
            v = slope = 0.0;
            return;
        }
        for (size_t i = 0; i < k.size(); i++) {
            if (std::fabs(k[i].t - t) <= KEY_EPSILON) {
                v = k[i].v;
                const double s = left ? k[i].in : k[i].out;
                slope = std::isfinite(s) ? s : 0.0;
                if (isStepped) {
                    if (left) *isStepped = i > 0 && (stepped(k[i - 1].out) || stepped(k[i].in));
                    else *isStepped = i + 1 < k.size() && (stepped(k[i].out) || stepped(k[i + 1].in));
                }
                return;
            }
        }
        if (t <= k.front().t || t >= k.back().t) {
            v = t <= k.front().t ? k.front().v : k.back().v;
            slope = 0.0;
            return;
        }
        size_t i = 0;
        while (i + 1 < k.size() && k[i + 1].t < t) i++;
        evalSegment(k[i], k[i + 1], t, v, slope);
        if (isStepped) *isStepped = stepped(k[i].out) || stepped(k[i + 1].in);
    }

    // the keys inside [start, stop], shifted to start at 0; added boundary keys carry the
    // curve's slope there (the same cubic)
    UCurve cropCurve(const UCurve& c, double start, double stop) {
        if (c.keys.empty()) return c;
        if (start == 0.0 && stop >= c.keys.back().t - KEY_EPSILON) return c;
        UCurve out;
        auto boundary = [&](double t, bool atStart) {
            for (const UKey& key : c.keys) {
                if (std::fabs(key.t - t) <= KEY_EPSILON) {
                    UKey copy = key;
                    copy.t = t - start;
                    out.keys.push_back(copy);
                    return;
                }
            }
            UKey key;
            key.t = t - start;
            double v, slope;
            bool isStepped = false;
            evalSided(c, t, false, v, slope, &isStepped);
            key.v = v;
            key.in = key.out = slope;
            if (isStepped && atStart) key.out = INFINITY;
            out.keys.push_back(key);
        };
        boundary(start, true);
        for (const UKey& key : c.keys) {
            if (key.t > start + KEY_EPSILON && key.t < stop - KEY_EPSILON) {
                UKey copy = key;
                copy.t -= start;
                out.keys.push_back(copy);
            }
        }
        if (stop > start + KEY_EPSILON) boundary(stop, false);
        return out;
    }

    // Unity's tangentMode (bit 0 broken, bits 1-4 left, bits 5-8 right: Free 0, Auto 1, Linear 2,
    // Constant 3, ClampedAuto 4) as the editor's mode
    ClipTangentMode tangentModeOf(int bits) {
        const int left = (bits >> 1) & 0xF;
        const int right = (bits >> 5) & 0xF;
        auto one = [](int m) {
            switch (m) {
                case 1: return ClipTangentMode::Auto;
                case 2: return ClipTangentMode::Linear;
                case 3: return ClipTangentMode::Constant;
                case 4: return ClipTangentMode::ClampedAuto;
                default: return ClipTangentMode::Free;
            }
        };
        if (left == right) return one(left);
        return (bits & 1) ? ClipTangentMode::Broken : ClipTangentMode::Free;
    }

    bool nodeFloat(const YAML::Node& node, double& out) {
        float f;
        if (!scalarFloat(node, f)) return false;
        out = f;
        return true;
    }

    // a curve's keys: component `component` of map values ({x, y, z, w}), or the scalar
    std::vector<UCurve> readUnityCurve(const YAML::Node& curveNode, const std::vector<const char*>& components) {
        std::vector<UCurve> out(std::max<size_t>(1, components.size()));
        const YAML::Node keys = curveNode && curveNode.IsMap() ? curveNode["m_Curve"] : YAML::Node();
        if (!keys || !keys.IsSequence()) return out;
        for (const YAML::Node& keyNode : keys) {
            if (!keyNode.IsMap()) continue;
            double t = 0.0;
            if (!nodeFloat(keyNode["time"], t)) continue;
            const int mode = readInt(keyNode, "tangentMode");
            const int weighted = readInt(keyNode, "weightedMode");
            for (size_t c = 0; c < out.size(); c++) {
                UKey key;
                key.t = t;
                key.mode = mode;
                key.weighted = weighted;
                auto part = [&](const char* name, double& value) {
                    const YAML::Node n = keyNode[name];
                    if (components.empty()) nodeFloat(n, value);
                    else if (n && n.IsMap()) nodeFloat(n[components[c]], value);
                };
                part("value", key.v);
                part("inSlope", key.in);
                part("outSlope", key.out);
                out[c].keys.push_back(key);
            }
        }
        for (UCurve& c : out) {
            std::stable_sort(c.keys.begin(), c.keys.end(), [](const UKey& a, const UKey& b) { return a.t < b.t; });
        }
        return out;
    }

    struct ImportNotes {
        Json list = Json::array();
        std::set<std::string> seen;
        void add(const std::string& code, const std::string& message) {
            if (!seen.insert(code + "\n" + message).second) return;
            list.push_back({{"code", code}, {"message", message}});
        }
    };

    std::vector<double> unionTimes(const std::vector<const UCurve*>& comps) {
        std::vector<double> times;
        for (const UCurve* c : comps) {
            if (!c) continue;
            for (const UKey& key : c->keys) times.push_back(key.t);
        }
        std::sort(times.begin(), times.end());
        std::vector<double> unique;
        for (double t : times) {
            if (unique.empty() || t - unique.back() > KEY_EPSILON) unique.push_back(t);
        }
        return unique;
    }

    // Unity keys' modes kept only where the engine's computed tangents equal the stored ones
    void verifyModes(ClipTrackData& track) {
        ClipTrackData computed = track;
        anim::applyTangentModes(computed);
        for (size_t k = 0; k < track.keys.size(); k++) {
            ClipKey& key = track.keys[k];
            if (key.tangentMode == ClipTangentMode::Free || key.tangentMode == ClipTangentMode::Broken ||
                key.tangentMode == ClipTangentMode::None) continue;
            const ClipKey& other = computed.keys[k];
            bool same = other.hasTangents() && other.inTangent.size() == key.inTangent.size();
            for (size_t c = 0; same && c < key.inTangent.size(); c++) {
                const double scale = std::max(1.0, (double)std::fabs(key.value[c]));
                same = std::fabs(other.inTangent[c] - key.inTangent[c]) <= 1e-4 * scale &&
                       std::fabs(other.outTangent[c] - key.outTangent[c]) <= 1e-4 * scale;
            }
            if (key.tangentMode == ClipTangentMode::Constant) same = same && key.ease == EaseType::STEP;
            if (!same) key.tangentMode = ClipTangentMode::Free;
        }
    }

    // per-component curves (null: the constant `defaults[c]`) as engine keys at the union of
    // their key times; `sign` mirrors values and slopes
    std::vector<ClipKey> mergeKeys(const std::vector<const UCurve*>& comps, const std::vector<double>& defaults,
                                   const std::vector<double>& sign, ImportNotes& notes, const std::string& what) {
        std::vector<ClipKey> keys;
        const std::vector<double> times = unionTimes(comps);
        bool mixedStep = false, weighted = false;
        for (double t : times) {
            ClipKey key;
            key.time = (float)t;
            key.value.resize(comps.size());
            key.inTangent.resize(comps.size());
            key.outTangent.resize(comps.size());
            bool allExact = true, anyAnimated = false, allStep = true, anyStep = false;
            int exactMode = -1;
            bool modesAgree = true;
            for (size_t c = 0; c < comps.size(); c++) {
                if (!comps[c] || comps[c]->empty()) {
                    key.value[c] = (float)defaults[c];
                    continue;
                }
                anyAnimated = true;
                const UCurve& curve = *comps[c];
                double v, inSlope, outSlope;
                bool stepIn = false, stepOut = false;
                evalSided(curve, t, true, v, inSlope, &stepIn);
                evalSided(curve, t, false, v, outSlope, &stepOut);
                key.value[c] = (float)(v * sign[c]);
                key.inTangent[c] = (float)(inSlope * sign[c]);
                key.outTangent[c] = (float)(outSlope * sign[c]);
                allStep = allStep && stepOut;
                anyStep = anyStep || stepOut;
                const UKey* exact = nullptr;
                for (const UKey& k : curve.keys) {
                    if (std::fabs(k.t - t) <= KEY_EPSILON) exact = &k;
                }
                if (!exact) {
                    allExact = false;
                } else {
                    weighted = weighted || exact->weighted != 0;
                    if (exactMode < 0) exactMode = exact->mode;
                    else if (tangentModeOf(exactMode) != tangentModeOf(exact->mode)) modesAgree = false;
                }
            }
            if (anyAnimated && allStep) key.ease = EaseType::STEP;
            if (anyStep && !allStep) mixedStep = true;
            key.tangentMode = allExact && modesAgree && exactMode >= 0 ? tangentModeOf(exactMode) : ClipTangentMode::Free;
            keys.push_back(std::move(key));
        }
        if (mixedStep) notes.add("mixed-step", what + ": some axes hold (Constant) where others move; those segments play smooth");
        if (weighted) notes.add("weighted-tangents", what + ": Unity's weighted tangents play as unweighted Hermite tangents");
        return keys;
    }

    // ---- Euler curves ----

    struct Quatd { double w = 1, x = 0, y = 0, z = 0; };

    Quatd mul(const Quatd& a, const Quatd& b) {
        return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    }

    Quatd axisQuat(int axis, double degrees) {
        const double half = degrees * 3.14159265358979323846 / 360.0;
        Quatd q;
        q.w = std::cos(half);
        (axis == 0 ? q.x : axis == 1 ? q.y : q.z) = std::sin(half);
        return q;
    }

    // Unity's RotationOrder (XYZ 0, XZY 1, YZX 2, YXZ 3, ZXY 4 = its default, ZYX 5): the first
    // letter's axis turns first
    Quatd eulerQuat(const double e[3], int order) {
        static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 2, 0}, {1, 0, 2}, {2, 0, 1}, {2, 1, 0}};
        const int* o = orders[(order >= 0 && order < 6) ? order : 4];
        Quatd q = axisQuat(o[0], e[o[0]]);
        q = mul(axisQuat(o[1], e[o[1]]), q);
        q = mul(axisQuat(o[2], e[o[2]]), q);
        return q;
    }

    // Euler degrees curves as quaternion keys at Unity's keys and every frame, the Euler
    // curves' derivative as the tangents
    std::vector<ClipKey> eulerKeys(const std::vector<const UCurve*>& comps, int order, bool mirrorX, double sampleRate, double length) {
        std::vector<double> times = unionTimes(comps);
        if (sampleRate > 0.0 && length > 0.0) {
            const int frames = (int)std::floor(length * sampleRate + 1e-6);
            for (int f = 0; f <= frames; f++) times.push_back(f / sampleRate);
            std::sort(times.begin(), times.end());
            std::vector<double> unique;
            for (double t : times) {
                if (unique.empty() || t - unique.back() > KEY_EPSILON) unique.push_back(t);
            }
            times.swap(unique);
        }
        const double sign[3] = {1.0, mirrorX ? -1.0 : 1.0, mirrorX ? -1.0 : 1.0};
        std::vector<ClipKey> keys;
        Quatd previous;
        bool havePrevious = false;
        for (double t : times) {
            double e[3], in[3], out[3];
            bool allStep = true, anyAnimated = false;
            for (int c = 0; c < 3; c++) {
                e[c] = in[c] = out[c] = 0.0;
                if (!comps[(size_t)c] || comps[(size_t)c]->empty()) continue;
                anyAnimated = true;
                bool stepOut = false;
                evalSided(*comps[(size_t)c], t, true, e[c], in[c]);
                evalSided(*comps[(size_t)c], t, false, e[c], out[c], &stepOut);
                allStep = allStep && stepOut;
                e[c] *= sign[c];
                in[c] *= sign[c];
                out[c] *= sign[c];
            }
            Quatd q = eulerQuat(e, order);
            auto derivative = [&](const double slope[3]) {
                const double h = 1e-4;
                double a[3], b[3];
                for (int c = 0; c < 3; c++) {
                    a[c] = e[c] + h * slope[c];
                    b[c] = e[c] - h * slope[c];
                }
                Quatd qa = eulerQuat(a, order), qb = eulerQuat(b, order);
                // the sample's hemisphere for both
                if (qa.w * q.w + qa.x * q.x + qa.y * q.y + qa.z * q.z < 0) qa = {-qa.w, -qa.x, -qa.y, -qa.z};
                if (qb.w * q.w + qb.x * q.x + qb.y * q.y + qb.z * q.z < 0) qb = {-qb.w, -qb.x, -qb.y, -qb.z};
                return Quatd{(qa.w - qb.w) / (2 * h), (qa.x - qb.x) / (2 * h), (qa.y - qb.y) / (2 * h), (qa.z - qb.z) / (2 * h)};
            };
            Quatd din = derivative(in), dout = derivative(out);
            if (havePrevious && q.w * previous.w + q.x * previous.x + q.y * previous.y + q.z * previous.z < 0) {
                q = {-q.w, -q.x, -q.y, -q.z};
                din = {-din.w, -din.x, -din.y, -din.z};
                dout = {-dout.w, -dout.x, -dout.y, -dout.z};
            }
            previous = q;
            havePrevious = true;
            ClipKey key;
            key.time = (float)t;
            key.value = {(float)q.x, (float)q.y, (float)q.z, (float)q.w};
            key.inTangent = {(float)din.x, (float)din.y, (float)din.z, (float)din.w};
            key.outTangent = {(float)dout.x, (float)dout.y, (float)dout.z, (float)dout.w};
            if (anyAnimated && allStep) {
                key.ease = EaseType::STEP;
                key.outTangent = {0, 0, 0, 0};
            }
            keys.push_back(std::move(key));
        }
        return keys;
    }

    // ---- bindings ----

    bool isMuscleAttribute(const std::string& a) {
        static const char* const prefixes[] = {"RootT.", "RootQ.", "MotionT.", "MotionQ.", "LeftFootT.", "LeftFootQ.", "RightFootT.",
                                               "RightFootQ.", "LeftHandT.", "LeftHandQ.", "RightHandT.", "RightHandQ.", "LeftHand.", "RightHand."};
        for (const char* p : prefixes) {
            if (a.rfind(p, 0) == 0) return true;
        }
        // the muscles' names have spaces ("Spine Front-Back", "Left Arm Down-Up")
        return a.find(' ') != std::string::npos;
    }

    std::string classLabel(int classId) {
        switch (classId) {
            case CLASS_GAMEOBJECT: return "GameObject";
            case CLASS_TRANSFORM: return "Transform";
            case CLASS_MESHRENDERER: return "MeshRenderer";
            case CLASS_SKINNEDMESHRENDERER: return "SkinnedMeshRenderer";
            case CLASS_SPRITERENDERER: return "SpriteRenderer";
            case CLASS_LIGHT: return "Light";
            case CLASS_CAMERA: return "Camera";
            case CLASS_AUDIOSOURCE: return "AudioSource";
            case CLASS_ANIMATOR: return "Animator";
            case CLASS_MONOBEHAVIOUR: return "MonoBehaviour";
            case CLASS_RECTTRANSFORM: return "RectTransform";
            default: return "class " + std::to_string(classId);
        }
    }

    // a Unity float binding as an engine property track's (component, property); false with
    // `reason` when nothing maps
    bool mapProperty(int classId, const std::string& attribute, std::string& component, std::string& property, std::string& reason) {
        std::string base = attribute, swizzle;
        const size_t dot = attribute.rfind('.');
        auto isSwizzle = [](const std::string& s) { return s == "r" || s == "g" || s == "b" || s == "a" || s == "x" || s == "y" || s == "z" || s == "w"; };
        if (dot != std::string::npos && isSwizzle(attribute.substr(dot + 1))) {
            base = attribute.substr(0, dot);
            swizzle = attribute.substr(dot);
        }
        if (classId == CLASS_GAMEOBJECT) {
            if (attribute != "m_IsActive") {
                reason = "only m_IsActive maps on a GameObject";
                return false;
            }
            component = "gameobject";
            property = "active";
        } else {
            std::string key;
            switch (classId) {
                case CLASS_MESHRENDERER:
                case CLASS_SKINNEDMESHRENDERER: key = "mesh"; break;
                case CLASS_SPRITERENDERER: key = base == "m_Enabled" ? "sprite" : "mesh"; break;
                case CLASS_LIGHT: key = "light"; break;
                case CLASS_CAMERA: key = "camera"; break;
                case CLASS_AUDIOSOURCE: key = "audiosource"; break;
                default:
                    reason = classId == CLASS_MONOBEHAVIOUR ? "script fields of Unity MonoBehaviours have no engine binding: key the Lua script's field "
                                                              "(script:<Class>) with edit_animation"
                                                            : "the component has no animatable engine property";
                    return false;
            }
            // a SpriteRenderer's colour is its quad's base colour
            const std::string unityName = classId == CLASS_SPRITERENDERER && base == "m_Color" ? "material._Color" : base;
            const anim::AnimatableProperty* row = nullptr;
            for (const anim::AnimatableProperty& candidate : anim::animatableProperties()) {
                if (key == candidate.component && unityName == candidate.unityName) {
                    row = &candidate;
                    break;
                }
            }
            if (row) {
                property = row->property;
                const size_t n = property.find("[N]");
                if (n != std::string::npos) property.replace(n, 3, "[0]");
            } else if (key == "mesh" && unityName.rfind("material.", 0) == 0) {
                // another material property: the shader's member of that name
                property = "submeshes[0].material.shaderParams." + unityName.substr(9);
            } else {
                reason = "no engine property ports " + base + " on a " + classLabel(classId);
                return false;
            }
            component = key;
            property += swizzle;
        }
        anim::PropertyRef ref;
        if (!anim::parsePropertyRef(component, property, ref) || !ref.known) {
            reason = ref.error.empty() ? "not an animatable property: " + component + "." + property : ref.error;
            return false;
        }
        return true;
    }

    std::string bindingName(const std::string& path, int classId, const std::string& attribute) {
        return (path.empty() ? std::string("(root)") : path) + ": " + classLabel(classId) + "." + attribute;
    }

    std::string trimUnityName(std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
        return s.substr(i);
    }

}

AnimImportResult jasperengine::editor::unity::importAnimText(const std::string& text, const AnimImportOptions& options) {
    AnimImportResult result;
    YamlFile file;
    std::string error;
    if (!file.parse(text, error)) {
        result.error = "not a Unity YAML file: " + error;
        return result;
    }
    const Document* clipDoc = nullptr;
    for (const Document& doc : file.documents) {
        if (doc.typeName == "AnimationClip") clipDoc = &doc;
    }
    if (!clipDoc || !clipDoc->body || !clipDoc->body.IsMap()) {
        result.error = "no AnimationClip in the file (a Unity .anim holds one AnimationClip document)";
        return result;
    }
    const YAML::Node body = clipDoc->body;
    const std::string sourceName = options.source.empty() ? readString(body, "m_Name") : options.source;
    ImportNotes notes;
    Json unmapped = Json::array();
    std::vector<std::string> muscles;
    ClipData& clip = result.clip;

    // settings: frame rate, range, wrap
    const double sampleRate = std::min(240.0, std::max(1.0, (double)readFloat(body, "m_SampleRate", 60.0f)));
    clip.frameRate = (float)sampleRate;
    const YAML::Node settings = body["m_AnimationClipSettings"];
    const bool hasSettings = settings && settings.IsMap();
    const bool legacy = readBool(body, "m_Legacy");

    // every curve, to find the clip's range
    struct VectorCurve {
        std::string path;
        ClipChannel channel;
        bool euler = false;
        int order = 4;
        std::vector<UCurve> comps;
    };
    std::vector<VectorCurve> vectors;
    auto readVectors = [&](const char* key, ClipChannel channel, bool euler, std::vector<const char*> components) {
        const YAML::Node list = body[key];
        if (!list || !list.IsSequence()) return;
        for (const YAML::Node& entry : list) {
            if (!entry.IsMap()) continue;
            VectorCurve v;
            v.path = readString(entry, "path");
            v.channel = channel;
            v.euler = euler;
            const YAML::Node curve = entry["curve"];
            v.order = curve && curve.IsMap() ? readInt(curve, "m_RotationOrder", 4) : 4;
            v.comps = readUnityCurve(curve, components);
            vectors.push_back(std::move(v));
        }
    };
    readVectors("m_PositionCurves", ClipChannel::Position, false, {"x", "y", "z"});
    readVectors("m_RotationCurves", ClipChannel::Rotation, false, {"x", "y", "z", "w"});
    readVectors("m_EulerCurves", ClipChannel::Rotation, true, {"x", "y", "z"});
    readVectors("m_ScaleCurves", ClipChannel::Scale, false, {"x", "y", "z"});

    struct FloatCurve {
        std::string path;
        std::string attribute;
        int classId = 0;
        UCurve curve;
    };
    std::vector<FloatCurve> floats;
    {
        const YAML::Node list = body["m_FloatCurves"];
        if (list && list.IsSequence()) {
            for (const YAML::Node& entry : list) {
                if (!entry.IsMap()) continue;
                FloatCurve f;
                f.path = readString(entry, "path");
                f.attribute = trimUnityName(readString(entry, "attribute"));
                f.classId = readInt(entry, "classID");
                f.curve = readUnityCurve(entry["curve"], {}).front();
                floats.push_back(std::move(f));
            }
        }
    }

    double lastKey = 0.0;
    for (const VectorCurve& v : vectors) {
        for (const UCurve& c : v.comps) {
            if (!c.keys.empty()) lastKey = std::max(lastKey, c.keys.back().t);
        }
    }
    for (const FloatCurve& f : floats) {
        if (!f.curve.keys.empty()) lastKey = std::max(lastKey, f.curve.keys.back().t);
    }
    double start = 0.0, stop = lastKey;
    if (hasSettings) {
        start = std::max(0.0, (double)readFloat(settings, "m_StartTime", 0.0f));
        stop = readFloat(settings, "m_StopTime", (float)lastKey);
        if (stop <= start) stop = std::max(start, lastKey);
    }
    const double length = stop - start;
    if (legacy) {
        // the legacy Animation component's WrapMode (Default 0, Once 1, Loop 2, PingPong 4, ClampForever 8)
        const int wrap = readInt(body, "m_WrapMode");
        clip.wrap = wrap == 2 ? ClipWrapMode::Loop : wrap == 4 ? ClipWrapMode::PingPong : wrap == 8 ? ClipWrapMode::ClampForever : ClipWrapMode::Once;
    } else {
        clip.wrap = hasSettings && readBool(settings, "m_LoopTime") ? ClipWrapMode::Loop : ClipWrapMode::Once;
    }
    clip.length = (float)length;

    std::set<std::string> rotationPaths;   // paths with quaternion curves (they win over Euler ones)
    for (const VectorCurve& v : vectors) {
        if (v.channel == ClipChannel::Rotation && !v.euler) rotationPaths.insert(v.path);
    }
    std::set<std::pair<std::string, int>> written;     // (path, channel)
    const bool mirror = options.mirrorX;

    auto addVectorTrack = [&](const std::string& path, ClipChannel channel, const std::vector<const UCurve*>& comps,
                              const std::vector<double>& defaults, bool euler, int order) {
        if (!written.insert({path, (int)channel}).second) {
            notes.add("duplicate-curve", (path.empty() ? std::string("(root)") : path) + ": a second " + anim::channelName(channel) +
                                         " curve for the same object was skipped");
            return;
        }
        std::vector<UCurve> cropped;
        for (const UCurve* c : comps) cropped.push_back(c ? cropCurve(*c, start, stop) : UCurve());
        std::vector<const UCurve*> parts;
        for (size_t c = 0; c < cropped.size(); c++) parts.push_back(comps[c] ? &cropped[c] : nullptr);
        ClipTrackData track;
        track.path = path;
        track.channel = channel;
        const std::string what = (path.empty() ? std::string("(root)") : path) + " " + anim::channelName(channel);
        if (euler) {
            track.keys = eulerKeys(parts, order, mirror, sampleRate, length);
        } else {
            std::vector<double> sign(comps.size(), 1.0);
            if (mirror && channel == ClipChannel::Position) sign[0] = -1.0;
            if (mirror && channel == ClipChannel::Rotation) sign[1] = sign[2] = -1.0;
            track.keys = mergeKeys(parts, defaults, sign, notes, what);
            verifyModes(track);
        }
        if (!track.keys.empty()) clip.tracks.push_back(std::move(track));
    };

    for (const VectorCurve& v : vectors) {
        if (v.euler && rotationPaths.count(v.path)) {
            notes.add("euler-and-quaternion", (v.path.empty() ? std::string("(root)") : v.path) +
                                              ": both quaternion and Euler curves; the quaternion curves were imported");
            continue;
        }
        std::vector<const UCurve*> comps;
        for (const UCurve& c : v.comps) comps.push_back(&c);
        const std::vector<double> defaults = v.channel == ClipChannel::Scale ? std::vector<double>{1, 1, 1} : std::vector<double>(comps.size(), 0.0);
        addVectorTrack(v.path, v.channel, comps, defaults, v.euler, v.order);
    }

    // Transform float curves, merged per object and kind
    struct TransformGroup {
        std::array<const UCurve*, 4> comps{};
        int count = 4;
        bool euler = false;
        ClipChannel channel = ClipChannel::Position;
    };
    std::map<std::pair<std::string, std::string>, TransformGroup> transformGroups;
    for (const FloatCurve& f : floats) {
        const std::string binding = bindingName(f.path, f.classId, f.attribute);
        if (f.classId == CLASS_TRANSFORM) {
            const size_t dot = f.attribute.rfind('.');
            const std::string base = dot == std::string::npos ? f.attribute : f.attribute.substr(0, dot);
            const std::string axis = dot == std::string::npos ? std::string() : f.attribute.substr(dot + 1);
            const int index = axis == "x" ? 0 : axis == "y" ? 1 : axis == "z" ? 2 : axis == "w" ? 3 : -1;
            TransformGroup group;
            if (base == "m_LocalPosition") {
                group.channel = ClipChannel::Position;
                group.count = 3;
            } else if (base == "m_LocalScale") {
                group.channel = ClipChannel::Scale;
                group.count = 3;
            } else if (base == "m_LocalRotation") {
                group.channel = ClipChannel::Rotation;
                group.count = 4;
            } else if (base == "localEulerAnglesRaw" || base == "localEulerAngles" || base == "localEulerAnglesBaked") {
                group.channel = ClipChannel::Rotation;
                group.count = 3;
                group.euler = true;
            } else {
                unmapped.push_back({{"binding", binding}, {"reason", "not a Transform channel"}});
                continue;
            }
            if (index < 0 || index >= group.count) {
                unmapped.push_back({{"binding", binding}, {"reason", "unknown axis"}});
                continue;
            }
            auto& slot = transformGroups[{f.path, base}];
            if (!slot.comps[0] && !slot.comps[1] && !slot.comps[2] && !slot.comps[3]) {
                slot.channel = group.channel;
                slot.count = group.count;
                slot.euler = group.euler;
            }
            slot.comps[(size_t)index] = &f.curve;
            continue;
        }
        if (f.classId == CLASS_ANIMATOR) {
            if (isMuscleAttribute(f.attribute)) {
                muscles.push_back(f.attribute);
            } else {
                unmapped.push_back({{"binding", binding}, {"reason", "an Animator parameter curve: set the parameter from a script or an event"}});
            }
            continue;
        }
        std::string component, property, reason;
        if (!mapProperty(f.classId, f.attribute, component, property, reason)) {
            unmapped.push_back({{"binding", binding}, {"reason", reason}});
            continue;
        }
        ClipTrackData track;
        track.path = f.path;
        track.channel = ClipChannel::Property;
        track.component = component;
        track.property = property;
        const UCurve cropped = cropCurve(f.curve, start, stop);
        if (component == "gameobject") {
            // a bool: its values, stepped
            for (const UKey& k : cropped.keys) {
                ClipKey key;
                key.time = (float)k.t;
                key.value = {k.v != 0.0 ? 1.0f : 0.0f};
                key.ease = EaseType::STEP;
                track.keys.push_back(key);
            }
        } else {
            track.keys = mergeKeys({&cropped}, {0.0}, {1.0}, notes, binding);
        }
        if (!track.keys.empty()) clip.tracks.push_back(std::move(track));
    }
    for (const auto& [id, group] : transformGroups) {
        std::vector<const UCurve*> comps(group.comps.begin(), group.comps.begin() + group.count);
        std::vector<double> defaults(comps.size(), group.channel == ClipChannel::Scale ? 1.0 : 0.0);
        if (group.channel == ClipChannel::Rotation && !group.euler) defaults[3] = 1.0;
        bool missing = false;
        for (const UCurve* c : comps) missing = missing || !c;
        if (missing) {
            notes.add("missing-axis", (id.first.empty() ? std::string("(root)") : id.first) + ": " + id.second +
                                      " has curves for some axes only; the others hold " + (group.channel == ClipChannel::Scale ? "1" : "0"));
        }
        if (group.channel == ClipChannel::Rotation && rotationPaths.count(id.first)) {
            notes.add("duplicate-curve", (id.first.empty() ? std::string("(root)") : id.first) + ": " + id.second +
                                         " float curves were skipped (the object has rotation curves)");
            continue;
        }
        addVectorTrack(id.first, group.channel, comps, defaults, group.euler, 4);
    }

    // object reference curves (sprite swaps, material swaps)
    {
        const YAML::Node list = body["m_PPtrCurves"];
        if (list && list.IsSequence()) {
            for (const YAML::Node& entry : list) {
                if (!entry.IsMap()) continue;
                unmapped.push_back({{"binding", bindingName(readString(entry, "path"), readInt(entry, "classID"), trimUnityName(readString(entry, "attribute")))},
                                    {"reason", "object reference curves (sprite or material swaps) are not imported: key sprite.frame with edit_animation"}});
            }
        }
    }

    // events
    {
        const YAML::Node list = body["m_Events"];
        if (list && list.IsSequence()) {
            for (const YAML::Node& entry : list) {
                if (!entry.IsMap()) continue;
                const double t = readFloat(entry, "time");
                const std::string function = trimUnityName(readString(entry, "functionName"));
                if (t < start - KEY_EPSILON || t > stop + KEY_EPSILON) {
                    notes.add("event-outside", "the event " + function + " is outside the clip's start/stop range: dropped");
                    continue;
                }
                if (!anim::isLuaIdentifier(function)) {
                    notes.add("event-name", "the event \"" + function + "\" is not a Lua function name: dropped");
                    continue;
                }
                ClipEventData event;
                event.time = (float)std::max(0.0, t - start);
                event.function = function;
                // Unity passes the one parameter the function declares: the string, else a
                // non-zero float, else a non-zero int
                const std::string data = readString(entry, "data");
                const float floatParameter = readFloat(entry, "floatParameter");
                const int intParameter = readInt(entry, "intParameter");
                if (!data.empty()) event.args.push_back(AnimationEvent::stringArg(data));
                else if (floatParameter != 0.0f) event.args.push_back(AnimationEvent::numberArg(floatParameter));
                else if (intParameter != 0) event.args.push_back(AnimationEvent::numberArg(intParameter));
                if (!toRef(entry["objectReferenceParameter"]).isNull()) {
                    notes.add("event-object", "the event " + function + "'s object parameter is not imported");
                }
                clip.events.push_back(std::move(event));
            }
        }
    }

    // Unity's root transform settings (S94 F32): Bake Into Pose, Based Upon and the offsets
    Json rootMotion;
    if (hasSettings) {
        const bool bakeRotation = readBool(settings, "m_LoopBlendOrientation");
        const bool bakeY = readBool(settings, "m_LoopBlendPositionY");
        const bool bakeXZ = readBool(settings, "m_LoopBlendPositionXZ");
        if (bakeRotation || bakeY || bakeXZ) {
            anim::ClipRootMotion block;
            block.bakeRotation = bakeRotation;
            block.bakePositionY = bakeY;
            block.bakePositionXZ = bakeXZ;
            block.rotationBasedUpon = readBool(settings, "m_KeepOriginalOrientation") ? anim::RootRotationBasis::Original
                                                                                       : anim::RootRotationBasis::BodyOrientation;
            block.rotationOffset = (float)readFloat(settings, "m_OrientationOffsetY");
            block.positionYBasedUpon = readBool(settings, "m_KeepOriginalPositionY") ? anim::RootPositionYBasis::Original
                                     : readBool(settings, "m_HeightFromFeet") ? anim::RootPositionYBasis::Feet
                                     : anim::RootPositionYBasis::CenterOfMass;
            block.positionYOffset = (float)readFloat(settings, "m_Level");
            block.positionXZBasedUpon = readBool(settings, "m_KeepOriginalPositionXZ") ? anim::RootPositionXZBasis::Original
                                                                                         : anim::RootPositionXZBasis::CenterOfMass;
            clip.unknownKeys.push_back({"root_motion", anim::writeClipRootMotion(block)});
            rootMotion = {{"bake_rotation", bakeRotation}, {"bake_position_y", bakeY}, {"bake_position_xz", bakeXZ},
                          {"rotation_based_upon", anim::rootRotationBasisName(block.rotationBasedUpon)},
                          {"rotation_offset", block.rotationOffset},
                          {"position_y_based_upon", anim::rootPositionYBasisName(block.positionYBasedUpon)},
                          {"position_y_offset", block.positionYOffset},
                          {"position_xz_based_upon", anim::rootPositionXZBasisName(block.positionXZBasedUpon)}};
        }
        if (readBool(settings, "m_Mirror")) notes.add("mirror", "the clip's Mirror setting is not imported: set Mirror on the Animator state");
        if (readFloat(settings, "m_CycleOffset") != 0.0f) notes.add("cycle-offset", "the clip's Cycle Offset is not imported: set it on the Animator state");
        if (readBool(settings, "m_HasAdditiveReferencePose")) {
            clip.additiveReference = (float)std::max(0.0, readFloat(settings, "m_AdditiveReferencePoseTime") - start);
        }
    }

    if (clip.tracks.empty() && clip.events.empty() && !muscles.empty()) {
        result.error = sourceName + " is a Unity humanoid (muscle-space) clip: its " + std::to_string(muscles.size()) +
                       " muscle curves have no JasperEngine equivalent (humanoid muscle clips are not imported). Use the clip of its "
                       "model instead (the .fbx with its Animation Type Humanoid plays through the humanoid retarget path), or bake it "
                       "to a Generic clip in Unity.";
        return result;
    }
    if (!muscles.empty()) {
        notes.add("muscle-curves", std::to_string(muscles.size()) + " humanoid muscle curves are not imported (muscle space has no "
                                   "JasperEngine equivalent); the Transform curves are");
    }

    std::vector<ClipWarning> warnings;
    anim::normalize(clip, &warnings);
    for (const ClipWarning& w : warnings) notes.add("normalized", (w.path.empty() ? std::string() : w.path + ": ") + w.message);

    Json muscleNames = Json::array();
    for (size_t i = 0; i < muscles.size() && i < 20; i++) muscleNames.push_back(muscles[i]);
    result.report = {{"source", sourceName},
                     {"name", readString(body, "m_Name")},
                     {"tracks", clip.tracks.size()},
                     {"events", clip.events.size()},
                     {"length", length},
                     {"wrap", anim::wrapName(clip.wrap)},
                     {"mirror_x", mirror},
                     {"unmapped", unmapped},
                     {"muscle_curves", {{"count", muscles.size()}, {"names", muscleNames}}},
                     {"notes", notes.list}};
    if (!rootMotion.is_null()) result.report["root_motion"] = rootMotion;
    result.ok = true;
    return result;
}

AnimImportResult jasperengine::editor::unity::importAnimFile(const std::filesystem::path& file, const AnimImportOptions& options) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        AnimImportResult result;
        result.error = "cannot read " + file.string();
        return result;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    AnimImportOptions withSource = options;
    if (withSource.source.empty()) withSource.source = file.filename().string();
    return importAnimText(buffer.str(), withSource);
}
