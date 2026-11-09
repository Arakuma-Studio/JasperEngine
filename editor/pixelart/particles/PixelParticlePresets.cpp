// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The presets' constants are pixelfx's (src/core/sim.js), MIT License, Copyright (c) 2026 Patch,
// adapted from Sprite Fusion's pixel art VFX, MIT License, Copyright (c) 2026 Hugo Duprez. See
// NOTICE.md. pixel_particle_presets.h is generated from editor/pixelart/particles by
// CMakeLists.txt; this is its only includer.

#include "PixelParticlePresets.h"

#include "particles/ParticleEffectCodec.h"
#include "particles/ParticleModules.h"
#include "pixel_particle_presets.h"
#include "pixelart/BuiltinPalettes.h"
#include "pixelart/PaletteRampTexture.h"
#include "pixelart/PixelColor.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::particles;

namespace jasperengine::editor::pixelart::presetio {
namespace {

    using Ordered = nlohmann::ordered_json;

    // the ramps of builtin:pixelfx-effects as a project path, or inline; children to their paths
    void rewrite(Ordered& j, const PixelPresetBuild& build, const jasperengine::pixelart::Palette* builtin, std::string& error){
        if (j.is_object()){
            if (j.contains("ramp") && j["ramp"].is_string()){
                const std::string ramp = j["ramp"].get<std::string>();
                const std::string prefix = std::string(kPixelPresetPalette) + "#ramp:";
                if (ramp.rfind(prefix, 0) == 0){
                    const std::string name = ramp.substr(prefix.size());
                    if (!build.palette.empty()){
                        j["ramp"] = build.palette + "#ramp:" + name;
                    }else{
                        Ordered colors = Ordered::array();
                        if (builtin){
                            for (const jasperengine::pixelart::Rgba8& c : builtin->rampColors(name)) colors.push_back(jasperengine::pixelart::toHex(c));
                        }
                        if (colors.empty()) error = "the built-in palette has no ramp '" + name + "'";
                        j.erase("ramp");
                        j["rampColors"] = colors;
                    }
                }
            }
            if (j.contains("effect") && j["effect"].is_string()){
                const std::string effect = j["effect"].get<std::string>();
                if (effect.rfind(kPresetPrefix, 0) == 0 && build.childPath)
                    j["effect"] = build.childPath(effect.substr(std::char_traits<char>::length(kPresetPrefix)));
            }
            for (auto it = j.begin(); it != j.end(); ++it) rewrite(it.value(), build, builtin, error);
        }else if (j.is_array()){
            for (Ordered& item : j) rewrite(item, build, builtin, error);
        }
    }

    void scaleCurve(MinMaxCurve& c, float f){
        c.constant *= f;
        c.constantMax *= f;
        c.multiplier *= f;
    }

    float rampDistance(const std::vector<jasperengine::pixelart::Rgba8>& a, const std::vector<jasperengine::pixelart::Rgba8>& b){
        // the two ramps resampled to the longer one's length, step by step in OKLab
        const int n = (int)std::max(a.size(), b.size());
        const std::vector<jasperengine::pixelart::Rgba8> fa = jasperengine::pixelart::fitRamp(a, n);
        const std::vector<jasperengine::pixelart::Rgba8> fb = jasperengine::pixelart::fitRamp(b, n);
        float sum = 0.0f;
        for (int i = 0; i < n; i++) sum += jasperengine::pixelart::distance(fa[(size_t)i], fb[(size_t)i], jasperengine::pixelart::ColorMetric::OkLab);
        return sum / (float)std::max(n, 1);
    }

    // every MinMaxGradient of the document's modules
    void eachGradient(ParticleEffectDocument& document, const std::function<void(MinMaxGradient&)>& visit){
        std::function<void(const StructDesc&, void*)> walk = [&](const StructDesc& desc, void* owner){
            for (const FieldDesc& field : desc.fields){
                if (field.type == FieldType::MinMaxGradient){
                    visit(*static_cast<MinMaxGradient*>(field.field(owner)));
                }else if (field.type == FieldType::Struct && field.element){
                    walk(*field.element, field.field(owner));
                }else if (field.type == FieldType::Array && field.element){
                    void* vector = field.field(owner);
                    for (size_t i = 0; i < field.array.size(vector); i++) walk(*field.element, field.array.at(vector, i));
                }
            }
        };
        for (const ModuleDesc& module : ParticleModules::all()) walk(module.fields, module.module(document));
    }

}
}

namespace pio = jasperengine::editor::pixelart::presetio;

const std::vector<std::string>& editor::pixelart::pixelParticlePresetNames(){
    static const std::vector<std::string> names = {"pixel_spark", "pixel_smoke", "pixel_fire", "pixel_ember", "pixel_grit",
                                                   "pixel_debris", "pixel_light_flash", "pixel_smoke_loop", "pixel_ember_loop"};
    return names;
}

std::string editor::pixelart::pixelParticlePresetLabel(const std::string& name){
    static const std::vector<std::pair<std::string, std::string>> labels = {
        {"pixel_spark", "Spark"}, {"pixel_smoke", "Smoke"}, {"pixel_fire", "Fire"}, {"pixel_ember", "Ember"}, {"pixel_grit", "Grit"},
        {"pixel_debris", "Debris"}, {"pixel_light_flash", "Light Flash"}, {"pixel_smoke_loop", "Smoke Loop"}, {"pixel_ember_loop", "Ember Loop"}};
    for (const auto& [key, label] : labels){
        if (key == name) return label;
    }
    return std::string();
}

std::string_view editor::pixelart::pixelParticlePresetText(std::string_view name){
    for (const PixelParticlePresetFile& file : pixel_particle_preset_files){
        if (file.name && name == file.name)
            return std::string_view(reinterpret_cast<const char*>(file.data), file.size);
    }
    return std::string_view();
}

std::vector<std::string> editor::pixelart::pixelParticlePresetChildren(const std::string& name){
    std::vector<std::string> children;
    const std::string_view text = pixelParticlePresetText(name);
    if (text.empty())
        return children;
    const pio::Ordered json = pio::Ordered::parse(text, nullptr, false);
    if (json.is_discarded() || !json.contains("subEmitters"))
        return children;
    for (const pio::Ordered& entry : json["subEmitters"]){
        const std::string effect = entry.value("effect", std::string());
        if (effect.rfind(kPresetPrefix, 0) == 0) children.push_back(effect.substr(std::char_traits<char>::length(kPresetPrefix)));
    }
    return children;
}

bool editor::pixelart::buildPixelParticlePreset(const std::string& name, const PixelPresetBuild& build, ParticleEffectDocument& out,
                                                std::string& error){
    const std::string_view text = pixelParticlePresetText(name);
    if (text.empty()){
        error = "no pixel preset '" + name + "'";
        return false;
    }
    pio::Ordered json = pio::Ordered::parse(text, nullptr, false);
    if (json.is_discarded()){
        error = "the preset '" + name + "' is not JSON";
        return false;
    }
    const BuiltinPalette* builtin = findBuiltinPalette(kPixelPresetPalette);
    pio::rewrite(json, build, builtin ? &builtin->palette : nullptr, error);
    if (!error.empty())
        return false;
    std::vector<Diagnostic> diagnostics;
    if (!ParticleEffectCodec::fromJson(json, out, diagnostics)){
        error = "the preset '" + name + "' does not read: " + ParticleEffectCodec::format(diagnostics);
        return false;
    }
    // base pixels per second into world units per second (S41 R6)
    const int ppu = std::max(build.pixelsPerUnit, 1);
    if (ppu != 1)
        scaleParticleMotion(out, 1.0f / (float)ppu);
    out.designPixelsPerUnit = (float)ppu;
    return true;
}

void editor::pixelart::scaleParticleMotion(ParticleEffectDocument& d, float f){
    pio::scaleCurve(d.main.startSpeed, f);
    const bool pixelSizes = d.pixelArt.present && d.pixelArt.enabled && d.pixelArt.pixelSizes;
    if (!pixelSizes){
        pio::scaleCurve(d.main.startSize, f);
        pio::scaleCurve(d.main.startSizeY, f);
        pio::scaleCurve(d.main.startSizeZ, f);
    }
    ShapeModule& s = d.shape;
    s.radius *= f;
    s.length *= f;
    s.donutRadius *= f;
    s.position = s.position * f;
    s.randomizePosition *= f;
    s.boxThickness = s.boxThickness * f;
    if (s.shape == ShapeType::Box || s.shape == ShapeType::Rectangle)
        s.scale = s.scale * f;
    for (MinMaxCurve* c : {&d.velocityOverLifetime.x, &d.velocityOverLifetime.y, &d.velocityOverLifetime.z, &d.velocityOverLifetime.radial,
                           &d.velocityOverLifetime.offsetX, &d.velocityOverLifetime.offsetY, &d.velocityOverLifetime.offsetZ,
                           &d.forceOverLifetime.x, &d.forceOverLifetime.y, &d.forceOverLifetime.z, &d.limitVelocity.limit,
                           &d.limitVelocity.limitY, &d.limitVelocity.limitZ, &d.noise.strength, &d.noise.strengthY, &d.noise.strengthZ,
                           &d.orbit.radialPull})
        pio::scaleCurve(*c, f);
    d.startVelocity.min = d.startVelocity.min * f;
    d.startVelocity.max = d.startVelocity.max * f;
    d.orbit.center = d.orbit.center * f;
    if (f > 0.0f) d.limitVelocity.quadraticDrag /= f;
    for (CollisionPlane& plane : d.collision.planes) plane.point = plane.point * f;
    d.collision.settleSpeed *= f;
    d.collision.minKillSpeed *= f;
    if (d.collision.maxKillSpeed < 10000.0f) d.collision.maxKillSpeed *= f;
}

bool editor::pixelart::matchPaletteRamps(ParticleEffectDocument& document, const jasperengine::pixelart::Palette& palette){
    if (palette.ramps.empty())
        return false;
    std::vector<std::vector<jasperengine::pixelart::Rgba8>> ramps;
    for (const jasperengine::pixelart::PaletteRamp& ramp : palette.ramps) ramps.push_back(palette.rampColors(ramp.name));
    pio::eachGradient(document, [&](MinMaxGradient& g){
        if (g.mode != GradientValueMode::Ramp)
            return;
        const std::vector<jasperengine::pixelart::Rgba8> colors = g.resolveRamp();
        if (colors.empty())
            return;
        size_t best = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (size_t r = 0; r < ramps.size(); r++){
            if (ramps[r].empty()) continue;
            const float d = pio::rampDistance(colors, ramps[r]);
            if (d < bestDistance){
                bestDistance = d;
                best = r;
            }
        }
        jasperengine::pixelart::PaletteMatcher matcher(ramps[best], jasperengine::pixelart::ColorMetric::OkLab);
        std::vector<jasperengine::pixelart::Rgba8> mapped;
        for (const jasperengine::pixelart::Rgba8& c : colors) mapped.push_back(ramps[best][(size_t)std::max(matcher.nearest(c), 0)]);
        // the steps as they are drawn now (fitted already)
        g.ramp.clear();
        g.rampSteps = 0;
        g.rampColors = mapped;
    });
    return true;
}
