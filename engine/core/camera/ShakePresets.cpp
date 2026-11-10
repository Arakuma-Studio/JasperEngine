// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShakePresets.h"

using namespace jasperengine;
using namespace jasperengine::camera;

namespace{

    ShakePreset make(ShakeType type, float strength, float roughness, float fadeIn, float fadeOut,
                     Vector3 positionInfluence, Vector3 rotationInfluence){
        ShakePreset preset;
        preset.type = type;
        preset.strength = strength;
        preset.roughness = roughness;
        preset.fadeIn = fadeIn;
        preset.fadeOut = fadeOut;
        preset.positionInfluence = positionInfluence;
        preset.rotationInfluence = rotationInfluence;
        return preset;
    }

    // S48 R22: a preset in art pixels, with its tick rate
    ShakePreset pixels(ShakePreset preset, float tick){
        preset.units = ShakeUnits::Pixels;
        preset.tick = tick;
        return preset;
    }

    constexpr ShakeType KICK = ShakeType::Kick;
    constexpr ShakeType ONE_SHOT = ShakeType::OneShot;
    constexpr ShakeType SUSTAINED = ShakeType::Sustained;

}

const std::vector<BuiltinShakePreset>& camera::builtinShakePresets(){
    static const std::vector<BuiltinShakePreset> presets = {
        // MilkShake's presets as the game ships them (Explosion.asset, LightExplosion.asset...)
        {"explosion", "A strong, short blast", make(ONE_SHOT, 0.2f, 15.0f, 0.05f, 1.0f, Vector3(1, 1, 0), Vector3(20, 20, 20))},
        {"light_explosion", "A lighter, shorter blast", make(ONE_SHOT, 0.1f, 12.0f, 0.05f, 1.0f, Vector3(1, 1, 0), Vector3(20, 20, 20))},
        {"earthquake", "A rumble that rolls the view", make(ONE_SHOT, 0.1f, 15.0f, 0.04f, 1.0f, Vector3(1, 1, 0), Vector3(0, 0, 30))},
        {"punch", "A slow, heavy sideways hit", make(ONE_SHOT, 0.5f, 2.5f, 0.05f, 1.0f, Vector3(4, 0, 0), Vector3(20, 90, 20))},
        {"shock", "A fast rotational jolt", make(ONE_SHOT, 0.05f, 50.0f, 0.0f, 0.5f, Vector3(0, 0, 0), Vector3(10, 60, 10))},
        {"electrocute", "A held, jittery rotation", make(SUSTAINED, 0.05f, 20.0f, 0.0f, 0.0f, Vector3(0, 0, 0), Vector3(0, 40, 40))},
        {"drunk", "A held, slow sway", make(SUSTAINED, 0.1f, 0.35f, 0.0f, 0.0f, Vector3(0, 0, 0), Vector3(5, 22.5f, 15))},
        {"offroad", "A held bumpy ride", make(SUSTAINED, 0.25f, 3.0f, 1.0f, 1.0f, Vector3(0.1f, 1, 0.1f), Vector3(20, 1, 1))},
        {"jumpscare_2d", "A held roll for a 2D jumpscare", make(SUSTAINED, 0.2f, 15.0f, 0.0f, 0.0f, Vector3(0, 0, 0), Vector3(0, 0, 40))},
        {"eh_shake", "A held, slow wobble with a 3 s fade out", make(SUSTAINED, 0.1f, 1.5f, 0.0f, 3.0f, Vector3(0.5f, 1, 0.5f), Vector3(7.5f, 2.5f, 7.5f))},
        {"entity_shake", "A 7 s build-up that fades out over 2 s", make(ONE_SHOT, 0.2f, 15.0f, 7.0f, 2.0f, Vector3(0, 0, 0), Vector3(20, 0, 20))},
        {"invader_drunk", "A held, gentle sway with a 2 s fade out", make(SUSTAINED, 0.1f, 0.5f, 0.0f, 2.0f, Vector3(0, 0, 0), Vector3(3, 3, 3))},
        {"invaders_explosion", "A sharp, short blast with a little roll", make(ONE_SHOT, 0.3f, 10.0f, 0.01f, 0.6f, Vector3(1, 1, 0), Vector3(0, 0, 3))},
        // EZ Camera Shake's (magnitude, roughness, fade in, fade out, position, rotation) with
        // strength = magnitude / 2: EZ's noise spans +-0.5 * magnitude, MilkShake's +-strength
        {"handheld", "A held, slow handheld-camera drift", make(SUSTAINED, 0.5f, 0.25f, 1.0f, 1.0f, Vector3(0, 0, 0), Vector3(1, 0.5f, 0.5f))},
        {"vibration", "A held, fine vibration", make(SUSTAINED, 0.2f, 20.0f, 0.5f, 0.5f, Vector3(0, 0.15f, 0), Vector3(1.25f, 0, 4))},
        {"bump", "A soft bump", make(ONE_SHOT, 1.25f, 4.0f, 0.1f, 0.75f, Vector3(0.15f, 0.15f, 0.15f), Vector3(1, 1, 1))},
        // S48's pixel-art shakes (pixelfx act.js): whole art pixels, no roll
        {"pixel_hit", "A 2-pixel kick over 0.15 s", pixels(make(KICK, 2.0f, 1.0f, 0.0f, 0.15f, Vector3(1, 1, 0), Vector3(0, 0, 0)), 0.0f)},
        {"pixel_heavy", "A 4-pixel kick over 0.35 s", pixels(make(KICK, 4.0f, 1.0f, 0.0f, 0.35f, Vector3(1, 1, 0), Vector3(0, 0, 0)), 0.0f)},
        {"pixel_rumble", "A held 1-pixel rumble stepping at 30 Hz", pixels(make(SUSTAINED, 1.0f, 8.0f, 0.0f, 0.2f, Vector3(1, 1, 0), Vector3(0, 0, 0)), 30.0f)},
    };
    return presets;
}

const ShakePreset* camera::findBuiltinShakePreset(const std::string& name){
    for (const BuiltinShakePreset& builtin : builtinShakePresets()){
        if (name == builtin.name) return &builtin.preset;
    }
    return nullptr;
}

std::vector<std::string> camera::builtinShakePresetNames(){
    std::vector<std::string> names;
    for (const BuiltinShakePreset& builtin : builtinShakePresets()){
        names.push_back(builtin.name);
    }
    return names;
}

const char* camera::defaultNewShakePresetName(){
    return "explosion";
}
