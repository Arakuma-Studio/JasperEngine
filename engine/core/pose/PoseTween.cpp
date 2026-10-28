// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pose/PoseTween.h"

#include "pose/PoseMath.h"

#include <cctype>
#include <cmath>

namespace jasperengine::pose {

    double easePrecise(double x, const Transition& transition){
        if (!(x > 0.0)){
            return 0.0;
        }
        switch (transition.curve){
            case TransitionCurve::Linear:
                return std::min(x, 1.0);
            case TransitionCurve::Smooth:
                return smootherstep(0.0, 1.0, x);
            case TransitionCurve::Spring:
            default:
                break;
        }
        // wind-up: a cosine dip to -a, reached with zero slope at x = p
        const double a = std::max(0.0, static_cast<double>(transition.anticipation));
        const double p = a > 0.0 ? 0.3 : 0.0;
        if (x < p){
            return -a * (1.0 - std::cos(POSE_PI * x / p)) / 2.0;
        }
        // underdamped step response, first crossing of 1 at u = 1, first peak 1 + Mp
        const double u = (x - p) / (1.0 - p);
        const double mp = clampd(transition.overshoot, 0.001, 0.9);
        const double k = -std::log(mp) / POSE_PI;
        const double w = POSE_PI - std::atan(1.0 / k);
        const double y = 1.0 - std::exp(-k * w * u) * (std::cos(w * u) + k * std::sin(w * u));
        return -a + (1.0 + a) * y;
    }

    float ease(float x, const Transition& transition){
        return static_cast<float>(easePrecise(x, transition));
    }

    Transition loosen(const Transition& transition, float looseness){
        Transition result = transition;
        result.duration = transition.duration * (1.0f + 1.5f * looseness);
        result.overshoot = transition.overshoot + (0.7f - transition.overshoot) * looseness;
        return result;
    }

    namespace {

        FeelPreset preset(const char* name, TransitionCurve curve, float duration, float overshoot, float anticipation,
                          float stepRate, float overlap, float looseness){
            Transition t;
            t.curve = curve;
            t.duration = duration;
            t.overshoot = overshoot;
            t.anticipation = anticipation;
            return FeelPreset{name, t, stepRate, overlap, looseness};
        }

        std::string presetKey(const std::string& name){
            std::string key;
            for (char c : name){
                if (c == '-' || c == '_' || c == ' '){
                    key += '-';
                }else{
                    key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            return key;
        }

        bool near(float a, float b){
            return std::fabs(a - b) <= 1e-4f;
        }

    }

    const std::vector<FeelPreset>& feelPresets(){
        using C = TransitionCurve;
        // spec: pose animation phase 1, table 4.8
        static const std::vector<FeelPreset> PRESETS = {
            preset("Snappy",      C::Spring, 0.12f, 0.25f, 0.06f, 0.0f,  0.05f, 0.2f),
            preset("Cartoon",     C::Spring, 0.10f, 0.35f, 0.12f, 0.0f,  0.10f, 0.65f),
            preset("Rubbery",     C::Spring, 0.20f, 0.50f, 0.10f, 0.0f,  0.10f, 0.5f),
            preset("Stop-motion", C::Spring, 0.10f, 0.20f, 0.0f,  12.0f, 0.04f, 0.0f),
            preset("Heavy",       C::Spring, 0.35f, 0.08f, 0.15f, 0.0f,  0.08f, 0.1f),
            preset("Linear",      C::Linear, 0.25f, 0.0f,  0.0f,  0.0f,  0.0f,  0.0f),
            preset("Soft",        C::Smooth, 0.30f, 0.0f,  0.0f,  0.0f,  0.06f, 0.15f),
        };
        return PRESETS;
    }

    const FeelPreset* findFeelPreset(const std::string& name){
        const std::string key = presetKey(name);
        for (const FeelPreset& p : feelPresets()){
            if (presetKey(p.name) == key){
                return &p;
            }
        }
        return nullptr;
    }

    bool feelPresetMatches(const FeelPreset& preset, const PoseAnimationSettings& settings){
        const Transition& a = preset.transition;
        const Transition& b = settings.transition;
        return a.curve == b.curve && near(a.duration, b.duration) && near(a.overshoot, b.overshoot) &&
               near(a.anticipation, b.anticipation) && near(preset.stepRate, settings.stepRate) &&
               near(preset.overlap, settings.overlap) && near(preset.looseness, settings.looseness);
    }

    const FeelPreset* matchingFeelPreset(const PoseAnimationSettings& settings){
        for (const FeelPreset& p : feelPresets()){
            if (feelPresetMatches(p, settings)){
                return &p;
            }
        }
        return nullptr;
    }

    void applyFeelPreset(const FeelPreset& preset, PoseAnimationSettings& settings){
        settings.transition = preset.transition;
        settings.stepRate = preset.stepRate;
        settings.overlap = preset.overlap;
        settings.looseness = preset.looseness;
    }

}
