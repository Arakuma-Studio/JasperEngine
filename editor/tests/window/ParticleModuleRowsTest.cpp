// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The module rows' document operations (docs/specs/S91-effect-authoring.md R22): the marks of
// values that are not their defaults, Reset of a field and of a module, Copy Module, Paste Module
// and Paste Values, and the help links.

#include "window/ParticleModuleRows.h"

#include "particles/ParticleEffectCodec.h"

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::particles;

TEST(ParticleModuleRows, OnlyValuesOffTheirDefaultsAreMarked) {
    ParticleEffectDocument d;
    EXPECT_TRUE(particlerows::nonDefaultFields(d).empty());
    d.duration = 2.0f;
    d.main.startSpeed = MinMaxCurve(12.0f);
    d.noise.present = true;          // a module added at its defaults marks nothing
    d.emission.enabled = false;      // a module's checkbox is not a field
    const std::set<std::string> marked = particlerows::nonDefaultFields(d);
    EXPECT_EQ(marked, (std::set<std::string>{"effect.duration", "main.startSpeed"}));
}

TEST(ParticleModuleRows, ResetPutsAFieldBack) {
    ParticleEffectDocument d;
    d.duration = 2.0f;
    d.main.startSpeed = MinMaxCurve(12.0f);
    d.main.gravityModifier = MinMaxCurve(1.0f);
    EXPECT_TRUE(particlerows::resetField(d, "main.startSpeed"));
    EXPECT_EQ(d.main.startSpeed, MinMaxCurve(5.0f));
    EXPECT_EQ(d.main.gravityModifier, MinMaxCurve(1.0f));
    EXPECT_TRUE(particlerows::resetField(d, "effect.duration"));
    EXPECT_FLOAT_EQ(d.duration, 5.0f);
    // already at its default, no such field, an absent module: nothing to do
    EXPECT_FALSE(particlerows::resetField(d, "effect.duration"));
    EXPECT_FALSE(particlerows::resetField(d, "main.startSpeeed"));
    EXPECT_FALSE(particlerows::resetField(d, "noise.strength"));
    EXPECT_FALSE(particlerows::resetField(d, "nonsense"));
}

TEST(ParticleModuleRows, ResetModuleKeepsItAndItsCheckbox) {
    ParticleEffectDocument d;
    d.noise.present = true;
    d.noise.enabled = false;
    d.noise.strength = MinMaxCurve(4.0f);
    d.noise.frequency = 2.0f;
    d.duration = 3.0f;
    ASSERT_TRUE(particlerows::resetModule(d, ModuleId::Noise));
    const ParticleEffectDocument defaults;
    EXPECT_TRUE(d.noise.present);
    EXPECT_FALSE(d.noise.enabled);
    EXPECT_EQ(d.noise.strength, defaults.noise.strength);
    EXPECT_FLOAT_EQ(d.noise.frequency, defaults.noise.frequency);
    // the rest of the effect untouched
    EXPECT_FLOAT_EQ(d.duration, 3.0f);
    EXPECT_FALSE(particlerows::resetModule(d, ModuleId::Noise));
    EXPECT_FALSE(particlerows::resetModule(d, ModuleId::Orbit));
}

TEST(ParticleModuleRows, CopyAndPasteAModule) {
    ParticleEffectDocument source;
    source.noise.present = true;
    source.noise.enabled = false;
    source.noise.strength = MinMaxCurve(4.0f);
    const std::string clip = particlerows::copyModule(source, ModuleId::Noise);
    const ModuleDesc* module = particlerows::clipboardModule(clip);
    ASSERT_NE(module, nullptr);
    EXPECT_EQ(module->id, ModuleId::Noise);
    EXPECT_EQ(particlerows::clipboardModule("plain text"), nullptr);
    EXPECT_EQ(particlerows::clipboardModule("{\"particleModule\": \"noize\", \"values\": {}}"), nullptr);

    // Paste Module adds the absent module as it was copied (its checkbox too)
    ParticleEffectDocument target;
    target.noise.frequency = 3.0f;   // not present: the module comes in as copied
    std::string error;
    ASSERT_TRUE(particlerows::pasteModule(target, clip, false, &error)) << error;
    EXPECT_TRUE(target.noise.present);
    EXPECT_FALSE(target.noise.enabled);
    EXPECT_EQ(target.noise.strength, MinMaxCurve(4.0f));
    EXPECT_FLOAT_EQ(target.noise.frequency, source.noise.frequency);

    // Paste Values keeps the target's checkbox, and needs the module there
    ParticleEffectDocument enabled;
    enabled.noise.present = true;
    enabled.noise.enabled = true;
    enabled.noise.scrollSpeed = MinMaxCurve(2.0f);
    ASSERT_TRUE(particlerows::pasteModule(enabled, clip, true, &error)) << error;
    EXPECT_TRUE(enabled.noise.enabled);
    EXPECT_EQ(enabled.noise.strength, MinMaxCurve(4.0f));
    EXPECT_EQ(enabled.noise.scrollSpeed, source.noise.scrollSpeed);
    ParticleEffectDocument without;
    EXPECT_FALSE(particlerows::pasteModule(without, clip, true, &error));
    EXPECT_NE(error.find("Noise"), std::string::npos) << error;
    EXPECT_FALSE(without.noise.present);
}

TEST(ParticleModuleRows, EveryModuleHasAHelpLink) {
    for (const ModuleDesc& module : ParticleModules::all()) {
        const std::string url = particlerows::moduleHelpUrl(module);
        EXPECT_EQ(url.rfind("https://github.com/JasperRaccoon/JasperEngine/blob/main/docs/editor/particle-effect.md#", 0), 0u) << url;
    }
    EXPECT_EQ(particlerows::moduleHelpUrl(ParticleModules::get(ModuleId::LimitVelocity)),
              "https://github.com/JasperRaccoon/JasperEngine/blob/main/docs/editor/particle-effect.md#limit-velocity-over-lifetime");
}
