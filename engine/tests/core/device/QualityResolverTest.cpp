// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// QualityResolver (spec S56 R3, R6, R7, R13, AC2, AC6): a tier's cells cap the project's values
// and empty ones inherit, the defaults leave High and Ultra alone, the thermal response steps up
// at once and recovers one state per 30 s of a simulated clock, memory pressure drops a mip, and
// the platform layers' entry points (Device::reportThermalState / reportLowMemory, what the JNI,
// NSProcessInfo and web glue call) reach it through Device::update on the main thread.

#include "device/Device.h"
#include "device/DeviceSettings.h"
#include "device/QualityResolver.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    class QualityResolverTest : public ::testing::Test {
    protected:
        void SetUp() override{
            Device::resetSession();
            QualityResolver::reset();
        }
        void TearDown() override{
            Device::reportThermalState(ThermalState::Nominal);
            Device::resetSession();
            Device::onThermalStateChanged.clear();
            Device::onLowMemory.clear();
            QualityResolver::reset();
        }

        static DeviceSettings mobileOnly(){
            DeviceSettings settings;
            settings.thermalResponse = {true, true, true};
            return settings;
        }
    };

}

// R13: without a tier (the editor's Auto, a test, a player before configure) nothing changes
TEST_F(QualityResolverTest, NoTierChangesNothing){
    QualityResolver::setSettings(DeviceSettings());
    EXPECT_FALSE(QualityResolver::getTier().has_value());
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 1.0f);
    EXPECT_EQ(QualityResolver::msaaSamples(4), 4);
    EXPECT_EQ(QualityResolver::shadowTier(ShadowTier::ULTRA), ShadowTier::ULTRA);
    EXPECT_FLOAT_EQ(QualityResolver::lodBias(), 1.0f);
    EXPECT_TRUE(QualityResolver::ssao(true));
    EXPECT_TRUE(QualityResolver::bloom(true));
    EXPECT_EQ(QualityResolver::volumetrics(AtmosphereQuality::HIGH), AtmosphereQuality::HIGH);
    EXPECT_EQ(QualityResolver::targetFrameRate(-1), -1);
    EXPECT_EQ(QualityResolver::textureQuality(1), 1);
    EXPECT_EQ(QualityResolver::particleBudget(1000u), 1000u);
}

// R3's defaults: Low halves, Low shadows, LOD Bias 0.5, no volumetrics or SSAO; Medium 0.85;
// High and Ultra are the project's values
TEST_F(QualityResolverTest, DefaultTiers){
    QualityResolver::setSettings(DeviceSettings());
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Mobile);

    QualityResolver::setTier(DeviceTier::Low);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 0.5f);
    EXPECT_EQ(QualityResolver::shadowTier(ShadowTier::HIGH), ShadowTier::LOW);
    EXPECT_FLOAT_EQ(QualityResolver::lodBias(), 0.5f);
    EXPECT_FALSE(QualityResolver::ssao(true));
    EXPECT_EQ(QualityResolver::volumetrics(AtmosphereQuality::MEDIUM), AtmosphereQuality::OFF);
    // empty cells inherit
    EXPECT_TRUE(QualityResolver::bloom(true));
    EXPECT_EQ(QualityResolver::msaaSamples(4), 4);

    QualityResolver::setTier(DeviceTier::Medium);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 0.85f);
    EXPECT_TRUE(QualityResolver::ssao(true));
    EXPECT_EQ(QualityResolver::shadowTier(ShadowTier::HIGH), ShadowTier::HIGH);

    for (DeviceTier tier : {DeviceTier::High, DeviceTier::Ultra}){
        QualityResolver::setTier(tier);
        EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 1.0f);
        EXPECT_EQ(QualityResolver::shadowTier(ShadowTier::ULTRA), ShadowTier::ULTRA);
        EXPECT_TRUE(QualityResolver::ssao(true));
        EXPECT_FLOAT_EQ(QualityResolver::lodBias(), 1.0f);
        EXPECT_TRUE(QualityResolver::getAppliedOverrides().empty());
    }
}

// a tier caps (never raises), Texture Quality drops at least, the LOD biases multiply
TEST_F(QualityResolverTest, CellsCapAndInherit){
    DeviceSettings settings;
    settings.lodBias = 2.0f;
    DeviceTierOverrides& high = settings.tier(DevicePlatformGroup::Desktop, DeviceTier::High);
    high.set(DeviceOverride::RenderScale, 0.75f);
    high.set(DeviceOverride::MsaaSamples, 2.0f);
    high.set(DeviceOverride::ShadowDistance, 40.0f);
    high.set(DeviceOverride::LodBias, 0.5f);
    high.set(DeviceOverride::TextureQuality, 1.0f);
    high.set(DeviceOverride::TargetFrameRate, 60.0f);
    high.set(DeviceOverride::MaxPixelLights, 4.0f);
    high.set(DeviceOverride::ParticleBudget, 5000.0f);
    high.set(DeviceOverride::LitQuality, 0.0f);
    high.set(DeviceOverride::Bloom, 0.0f);
    QualityResolver::setSettings(settings);
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Desktop);
    QualityResolver::setTier(DeviceTier::High);

    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 0.75f);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(0.6f), 0.6f) << "a cap never raises the scene's value";
    EXPECT_EQ(QualityResolver::msaaSamples(4), 2);
    EXPECT_EQ(QualityResolver::msaaSamples(1), 1);
    EXPECT_FLOAT_EQ(QualityResolver::shadowDistance(150.0f), 40.0f);
    EXPECT_FLOAT_EQ(QualityResolver::shadowDistance(20.0f), 20.0f);
    EXPECT_FLOAT_EQ(QualityResolver::lodBias(), 1.0f);
    EXPECT_EQ(QualityResolver::textureQuality(0), 1);
    EXPECT_EQ(QualityResolver::textureQuality(2), 2);
    EXPECT_EQ(QualityResolver::targetFrameRate(-1), 60);
    EXPECT_EQ(QualityResolver::targetFrameRate(120), 60);
    EXPECT_EQ(QualityResolver::targetFrameRate(30), 30);
    EXPECT_EQ(QualityResolver::maxPixelLights(8), 4);
    EXPECT_EQ(QualityResolver::particleBudget(65536u), 5000u);
    EXPECT_EQ(QualityResolver::litQuality(LitQuality::HIGH), LitQuality::STANDARD);
    EXPECT_FALSE(QualityResolver::bloom(true));
    EXPECT_FALSE(QualityResolver::bloom(false));
    // the cells not set inherit
    EXPECT_TRUE(QualityResolver::ssao(true));
    EXPECT_EQ(QualityResolver::shadowTier(ShadowTier::ULTRA), ShadowTier::ULTRA);
    // another group's table does not apply
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Web);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 1.0f);
}

TEST_F(QualityResolverTest, RevisionGoesUpOnChangesOnly){
    const uint32_t start = QualityResolver::getRevision();
    QualityResolver::setTier(std::nullopt);
    QualityResolver::setSettings(DeviceSettings());
    EXPECT_EQ(QualityResolver::getRevision(), start) << "nothing changed";
    QualityResolver::setTier(DeviceTier::Low);
    EXPECT_GT(QualityResolver::getRevision(), start);
}

// R6 and AC2: Serious lowers the scale by 20 % and caps at 30 fps, Critical adds the tier below's
// shadows and post-processing, recovery after 30 s per state (hysteresis), on a simulated clock
TEST_F(QualityResolverTest, ThermalStepsAndRecovery){
    QualityResolver::setSettings(mobileOnly());
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Mobile);
    QualityResolver::setTier(DeviceTier::Medium);

    QualityResolver::setThermalState(ThermalState::Fair);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Fair);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 0.85f) << "Fair changes nothing";

    QualityResolver::setThermalState(ThermalState::Serious);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 0.85f * 0.8f);
    EXPECT_EQ(QualityResolver::targetFrameRate(-1), 30);
    EXPECT_EQ(QualityResolver::targetFrameRate(60), 30);
    EXPECT_EQ(QualityResolver::targetFrameRate(24), 24) << "a target at or below 30 stays";
    EXPECT_TRUE(QualityResolver::ssao(true)) << "Serious keeps the shadows and post-processing";

    QualityResolver::setThermalState(ThermalState::Critical);
    // Low's cells for shadows and post-processing, Medium's for the rest
    EXPECT_FALSE(QualityResolver::ssao(true));
    EXPECT_EQ(QualityResolver::volumetrics(AtmosphereQuality::HIGH), AtmosphereQuality::OFF);
    EXPECT_EQ(QualityResolver::shadowTier(ShadowTier::HIGH), ShadowTier::LOW);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 0.85f * 0.8f) << "the render scale is Medium's, scaled";
    EXPECT_FLOAT_EQ(QualityResolver::lodBias(), 1.0f) << "LOD Bias is content: Critical leaves it";

    // the device cools down: nothing changes for 30 s, then one state a step
    QualityResolver::setThermalState(ThermalState::Nominal);
    for (int i = 0; i < 29; i++)
        QualityResolver::update(1.0);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Critical);
    EXPECT_NEAR(QualityResolver::getRecoverySeconds(), 1.0, 1e-6);
    QualityResolver::update(1.0);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Serious);
    EXPECT_TRUE(QualityResolver::ssao(true));
    EXPECT_EQ(QualityResolver::targetFrameRate(-1), 30);

    // heating up again resets the clock (hysteresis); the step up is immediate
    QualityResolver::update(20.0);
    QualityResolver::setThermalState(ThermalState::Serious);
    QualityResolver::setThermalState(ThermalState::Nominal);
    QualityResolver::update(20.0);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Serious) << "20 s since it cooled again";
    QualityResolver::update(10.0);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Fair);
    QualityResolver::update(30.0);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Nominal);
    EXPECT_EQ(QualityResolver::targetFrameRate(-1), -1);
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 0.85f);
}

TEST_F(QualityResolverTest, ThermalResponseFollowsTheGroupsSetting){
    QualityResolver::setSettings(DeviceSettings());
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Desktop);
    QualityResolver::setThermalState(ThermalState::Serious);
    EXPECT_EQ(QualityResolver::getReportedThermalState(), ThermalState::Serious);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Nominal) << "desktop: off by default";
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 1.0f);
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Mobile);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Serious) << "mobile: on by default";
    EXPECT_FLOAT_EQ(QualityResolver::renderScale(1.0f), 0.8f);
}

TEST_F(QualityResolverTest, MemoryPressureDropsAMip){
    QualityResolver::addMemoryPressure();
    EXPECT_EQ(QualityResolver::textureQuality(0), 1);
    EXPECT_EQ(QualityResolver::textureQuality(3), 3);
    QualityResolver::addMemoryPressure();
    EXPECT_EQ(QualityResolver::getMemoryDrop(), QualityResolver::MAX_MEMORY_DROP);
    QualityResolver::resetResponse();
    EXPECT_EQ(QualityResolver::textureQuality(0), 0);
}

// AC6: the platform glue's entry points, from another thread, reach the response and the events
// on the next Device::update (the main thread's frame)
TEST_F(QualityResolverTest, PlatformSignalsReachTheResponse){
    QualityResolver::setSettings(mobileOnly());
    QualityResolver::setPlatformGroup(DevicePlatformGroup::Mobile);
    std::vector<int> states;
    int lowMemory = 0;
    Device::onThermalStateChanged.add("test_thermal", [&states](int state) { states.push_back(state); });
    Device::onLowMemory.add("test_low_memory", [&lowMemory]() { lowMemory++; });

    std::thread platform([](){
        Device::reportThermalState(ThermalState::Critical);
        Device::reportLowMemory();
        Device::reportLowMemory();
    });
    platform.join();
    EXPECT_TRUE(states.empty()) << "nothing happens off the main thread";
    Device::update(1.0 / 60.0);
    ASSERT_EQ(states.size(), 1u);
    EXPECT_EQ(states[0], (int)ThermalState::Critical);
    EXPECT_EQ(Device::getThermalState(), ThermalState::Critical);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Critical);
    EXPECT_EQ(lowMemory, 1) << "warnings of one frame are one response";
    EXPECT_EQ(Device::getLowMemoryCount(), 1);
    EXPECT_EQ(QualityResolver::getMemoryDrop(), 1);

    Device::reportThermalState(ThermalState::Nominal);
    Device::update(1.0);
    ASSERT_EQ(states.size(), 2u);
    EXPECT_EQ(states[1], (int)ThermalState::Nominal);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Critical) << "the response waits 30 s";
    for (int i = 0; i < 31; i++)
        Device::update(1.0);
    EXPECT_EQ(QualityResolver::getThermalResponse(), ThermalState::Serious);
}

// the simulation (the editor's Game view) wins over the device; Play stop ends it
TEST_F(QualityResolverTest, SimulationAndSetTier){
    DeviceDetection detection;
    detection.detected = true;
    detection.tier = DeviceTier::High;
    Device::setDetection(detection);
    EXPECT_FALSE(Device::isTierApplied()) << "not configured: the editor applies no detected tier";
    Device::setTier(DeviceTier::Medium);
    EXPECT_EQ(QualityResolver::getTier(), DeviceTier::Medium);
    DeviceSimulation simulation;
    simulation.tier = DeviceTier::Low;
    simulation.group = DevicePlatformGroup::Web;
    simulation.thermal = ThermalState::Serious;
    Device::setSimulation(simulation);
    Device::update(0.0);
    EXPECT_EQ(Device::getTier(), DeviceTier::Low);
    EXPECT_EQ(QualityResolver::getPlatformGroup(), DevicePlatformGroup::Web);
    EXPECT_EQ(Device::getThermalState(), ThermalState::Serious);
    Device::resetSession();
    EXPECT_FALSE(Device::isTierApplied());
    EXPECT_EQ(Device::getTier(), DeviceTier::High);
    EXPECT_EQ(Device::getThermalState(), ThermalState::Nominal);
}

// R13 and the codec of main.cpp and game.json: only what differs is written, a missing key keeps
// its default, a cleared default cell stays cleared
TEST_F(QualityResolverTest, SettingsJsonRoundTrip){
    EXPECT_EQ(DeviceSettings().toJson(), "{}");
    DeviceSettings settings;
    settings.tier(DevicePlatformGroup::Mobile, DeviceTier::Low).set(DeviceOverride::RenderScale, std::nullopt);
    settings.tier(DevicePlatformGroup::Web, DeviceTier::High).set(DeviceOverride::TargetFrameRate, 30.0f);
    settings.lodBias = 0.75f;
    settings.thermalResponse[0] = true;
    settings.uploadBudgetMB[2] = 4;
    DeviceSettings back;
    std::string error;
    ASSERT_TRUE(DeviceSettings::fromJson(settings.toJson(), back, error)) << error;
    EXPECT_EQ(back, settings);
    EXPECT_FALSE(back.tier(DevicePlatformGroup::Mobile, DeviceTier::Low).get(DeviceOverride::RenderScale).has_value());
    EXPECT_TRUE(back.tier(DevicePlatformGroup::Desktop, DeviceTier::Low).get(DeviceOverride::RenderScale).has_value());

    EXPECT_FALSE(DeviceSettings::fromJson("{\"tiers\": {\"mobile\": {\"low\": {\"render_scale\": 3}}}}", back, error));
    EXPECT_FALSE(DeviceSettings::fromJson("{\"tiers\": {\"mobile\": {\"low\": {\"ssao\": true}}}}", back, error));
    EXPECT_FALSE(DeviceSettings::fromJson("{\"upload_budget_mb\": {\"web\": 0}}", back, error));
    // a newer player's cell is ignored
    ASSERT_TRUE(DeviceSettings::fromJson("{\"tiers\": {\"mobile\": {\"low\": {\"future_cell\": 1}}}}", back, error)) << error;
    EXPECT_EQ(back, DeviceSettings());
}
