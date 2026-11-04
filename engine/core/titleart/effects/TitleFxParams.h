// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.5)
// - Purpose: the builders the definition files (effects/*.cpp, generators/*.cpp) write their
//   ParamDef tables with (Pachislop's pNum, pPx, pDeg, pColor, pSelect... of src/effects/a/
//   util.ts), so each param states key, label, range, default, unit, one-line doc and its
//   animation class in one line; and the shared option lists (the 18 blend modes, the Material
//   pattern kinds).
// - Classes (S85 R11, S80 §6.5 "before S83 lands"): numbers and colours re-render smoothly
//   (FxAnim::Rerender); enums, bools, integers, seeds, assets and layers step (FxAnim::Step,
//   ParamInterp::Stepped).
// - Thread-safety: pure functions.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "titleart/TitleEffectRegistry.h"

#include <initializer_list>
#include <vector>

namespace jasperengine::titleart::fxparams {

    inline ParamDef number(const char* key, const char* label, double value, double min, double max, ParamUnit unit, const char* doc,
                           double step = 0.01) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::Number;
        p.unit = unit;
        p.min = min;
        p.max = max;
        p.step = step;
        p.defaultValue = value;
        p.description = doc;
        return p;
    }

    inline ParamDef px(const char* key, const char* label, double value, double min, double max, const char* doc, double step = 1.0) {
        return number(key, label, value, min, max, ParamUnit::Px, doc, step);
    }

    inline ParamDef degrees(const char* key, const char* label, double value, const char* doc, double min = -180.0, double max = 360.0) {
        return number(key, label, value, min, max, ParamUnit::Deg, doc, 1.0);
    }

    inline ParamDef integer(const char* key, const char* label, int value, int min, int max, const char* doc) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::Integer;
        p.min = min;
        p.max = max;
        p.step = 1;
        p.defaultValue = value;
        p.description = doc;
        p.anim = FxAnim::Step;
        p.interp = ParamInterp::Stepped;
        return p;
    }

    inline ParamDef color(const char* key, const char* label, const char* value, const char* doc, bool light = false) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::Color;
        p.defaultValue = value;
        p.description = doc;
        p.light = light;
        return p;
    }

    inline ParamDef flag(const char* key, const char* label, bool value, const char* doc) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::Bool;
        p.defaultValue = value;
        p.description = doc;
        p.anim = FxAnim::Step;
        p.interp = ParamInterp::Stepped;
        return p;
    }

    inline ParamDef choice(const char* key, const char* label, const char* value, std::vector<EnumOption> options, const char* doc) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::Enum;
        p.defaultValue = value;
        p.options = std::move(options);
        p.description = doc;
        p.anim = FxAnim::Step;
        p.interp = ParamInterp::Stepped;
        return p;
    }

    // a gradient param: stops of (pos, "#rrggbb") at alpha 1 and midpoint 0.5
    inline Json gradientValue(std::initializer_list<std::pair<double, const char*>> stops) {
        Json list = Json::array();
        for (const auto& s : stops) list.push_back(Json{{"pos", s.first}, {"color", s.second}, {"alpha", 1}, {"mid", 0.5}});
        return Json{{"stops", list}, {"interp", "rgb"}};
    }

    inline ParamDef gradient(const char* key, const char* label, Json value, const char* doc, bool light = false) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::Gradient;
        p.defaultValue = std::move(value);
        p.description = doc;
        p.light = light;
        return p;
    }

    inline ParamDef asset(const char* key, const char* label, const char* doc) {
        ParamDef p;
        p.key = key;
        p.label = label;
        p.kind = ParamKind::Asset;
        p.defaultValue = nullptr;
        p.description = doc;
        p.anim = FxAnim::Step;
        p.interp = ParamInterp::Stepped;
        return p;
    }

    inline ParamDef when(ParamDef p, const char* param, std::vector<const char*> values) {
        p.when.param = param;
        p.when.values = std::move(values);
        return p;
    }

    // a seed: an integer the document's seed adds to (R17), stepped
    inline ParamDef seed(const char* key, const char* label, const char* doc) {
        return integer(key, label, 1, 0, 999, doc);
    }

    // the 18 blend modes of §6.4 in TitleBlend order (their ids are the enum values 0..17)
    inline std::vector<EnumOption> blendOptions() {
        std::vector<EnumOption> options;
        for (int i = 0; i < TITLE_BLEND_COUNT; i++) options.push_back({titleBlendName((TitleBlend)i), titleBlendLabel((TitleBlend)i)});
        return options;
    }

    // the Material pattern kinds (§6.1 Material `pattern.kind`), `none` and `custom` left to the caller
    inline std::vector<EnumOption> patternOptions() {
        return {{"zebra", "Zebra"}, {"tiger", "Tiger"}, {"giraffe", "Giraffe"}, {"leopard", "Leopard"}, {"checker", "Checker"},
                {"hazard", "Hazard"}, {"stripes", "Stripes"}, {"dots", "Polka Dots"}, {"sakura", "Sakura"}, {"seigaiha", "Seigaiha"},
                {"asanoha", "Asanoha"}, {"shippo", "Shippo"}, {"kikko", "Kikko"}, {"uroko", "Uroko"}};
    }

    // the index of the param's current option (0 when unknown)
    inline int optionIndex(const ParamValues& p, const char* key, std::initializer_list<const char*> values) {
        const std::string v = p.string(key);
        int i = 0;
        for (const char* value : values) {
            if (v == value) return i;
            i++;
        }
        return 0;
    }

}
