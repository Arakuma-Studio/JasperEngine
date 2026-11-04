// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Device.h"

#include "Engine.h"
#include "Log.h"
#include "System.h"
#include "device/QualityResolver.h"
#include "render/SystemRender.h"

#include <atomic>

using namespace jasperengine;

FunctionSubscribe<void(int)> Device::onThermalStateChanged;
FunctionSubscribe<void()> Device::onLowMemory;

namespace {

    struct DeviceRunState {
        DeviceDetection detection;
        bool configured = false;
        std::optional<DeviceTier> setTier;
        DeviceSimulation simulation;
        ThermalState thermal = ThermalState::Nominal;
        ThermalState response = ThermalState::Nominal;
        int lowMemoryCount = 0;
    };

    DeviceRunState& deviceRunState(){
        static DeviceRunState* s = new DeviceRunState();
        return *s;
    }

    // written by the platform layers on their threads, read once a frame
    std::atomic<int> devicePlatformThermal{(int)ThermalState::Nominal};
    std::atomic<int> devicePendingLowMemory{0};

    const char* deviceDatabasePlatform(Platform platform){
        switch (platform){
            case Platform::Android: return "android";
            case Platform::iOS:
            case Platform::MacOS: return "apple";
            case Platform::Web: return "web";
            default: return "pc";
        }
    }

    const char* devicePlatformLabel(Platform platform){
        switch (platform){
            case Platform::Android: return "Android";
            case Platform::iOS: return "iOS";
            case Platform::MacOS: return "macOS";
            case Platform::Web: return "the web";
            case Platform::Linux: return "Linux";
            case Platform::Windows: return "Windows";
        }
        return "desktop";
    }

    // R2: a GPU the database does not know
    DeviceTier deviceHeuristicTier(const DeviceSpecs& profile, std::string& why){
        const Platform platform = (Platform)profile.platform;
        const int mb = profile.memoryMB;
        why = std::string(devicePlatformLabel(platform)) + " with " + (mb > 0 ? std::to_string(mb) + " MB" : std::string("unknown memory")) +
              " and " + std::to_string(profile.cores) + " cores";
        switch (platform){
            case Platform::Android:
            case Platform::iOS:
                if (mb <= 0) return DeviceTier::Medium;
                if (mb < 3 * 1024) return DeviceTier::Low;
                if (mb < 6 * 1024) return DeviceTier::Medium;
                return DeviceTier::High;
            case Platform::Web:
                // navigator.deviceMemory (rounded, at most 8 GB; absent in Firefox and Safari)
                return (mb > 0 && mb < 4 * 1024) ? DeviceTier::Low : DeviceTier::Medium;
            default:
                return (profile.cores > 0 && profile.cores <= 2) ? DeviceTier::Medium : DeviceTier::High;
        }
    }

    void logThermalResponse(ThermalState response){
        switch (response){
            case ThermalState::Nominal:
            case ThermalState::Fair:
                Log::print("Thermal response: %s, the device tier's settings apply again", DeviceNames::thermalLabel(response));
                break;
            case ThermalState::Serious:
                Log::warn("Thermal response: Serious, render scale x%.1f and at most %d fps", QualityResolver::THERMAL_SCALE,
                          QualityResolver::THERMAL_FRAME_CAP);
                break;
            case ThermalState::Critical:
                Log::warn("Thermal response: Critical, render scale x%.1f, at most %d fps and the tier below's shadows and post-processing",
                          QualityResolver::THERMAL_SCALE, QualityResolver::THERMAL_FRAME_CAP);
                break;
        }
    }

}

// ---------------------------------------------------------------- detection

DeviceDetection Device::detectTier(const DeviceSpecs& profile, const GpuTierDatabase& database){
    DeviceDetection detection;
    detection.detected = true;
    detection.profile = profile;

    GpuQuery query;
    query.renderer = profile.gpuName;
    query.platform = deviceDatabasePlatform((Platform)profile.platform);
    const GpuMatch match = database.match(query);
    detection.vendor = match.vendor;
    const std::string gpu = profile.gpuName.empty() ? std::string("No GPU name") : profile.gpuName;
    if (match.matched){
        detection.tier = match.tier;
        detection.kind = match.kind;
        detection.rule = match.rule;
        detection.reason = gpu + ", rule " + match.kind + " (" + match.rule + "), tier " + DeviceNames::tierLabel(match.tier);
    }else{
        std::string why;
        detection.tier = deviceHeuristicTier(profile, why);
        detection.kind = "heuristic";
        detection.rule = why;
        detection.reason = gpu + ", unknown GPU" + (match.vendor.empty() ? std::string() : " (" + match.vendor + ")") +
                           ", heuristic: " + why + ", tier " + DeviceNames::tierLabel(detection.tier);
    }
    return detection;
}

void Device::detect(){
    DeviceSpecs profile;
    profile.platform = (int)Engine::getPlatform();
    profile.backend = SystemRender::getBackendName();
    if (System::hasInstance()){
        System& system = System::instance();
        // the web's unmasked WebGL renderer (GL_RENDERER is the browser's generic one)
        profile.gpuName = system.getGpuRendererName();
        profile.memoryMB = system.getSystemMemorySize();
        profile.cores = system.getProcessorCount();
    }
    if (profile.gpuName.empty())
        profile.gpuName = SystemRender::getDeviceName();
    setDetection(detectTier(profile, GpuTierDatabase::builtIn()));
    const DeviceRunState& s = deviceRunState();
    Log::print("Device tier: %s (%s)%s", s.detection.reason.c_str(), profile.backend.c_str(),
               s.configured ? "" : "; not applied (the editor applies a simulated or set tier only)");
}

void Device::setDetection(const DeviceDetection& detection){
    deviceRunState().detection = detection;
    applyTier();
}

const DeviceDetection& Device::getDetection(){
    return deviceRunState().detection;
}

void Device::configure(const DeviceSettings& settings){
    deviceRunState().configured = true;
    QualityResolver::setSettings(settings);
    applyTier();
}

void Device::configure(const char* settingsJson){
    DeviceSettings settings;
    std::string error;
    if (settingsJson && !DeviceSettings::fromJson(settingsJson, settings, error)){
        Log::error("Device settings: %s; the defaults apply", error.c_str());
        settings = DeviceSettings();
    }
    configure(settings);
}

bool Device::isConfigured(){
    return deviceRunState().configured;
}

void Device::setProjectSettings(const DeviceSettings& settings){
    QualityResolver::setSettings(settings);
}

// ---------------------------------------------------------------- the tier

void Device::applyTier(){
    const DeviceRunState& s = deviceRunState();
    std::optional<DeviceTier> tier;
    if (s.simulation.tier)
        tier = s.simulation.tier;
    else if (s.setTier)
        tier = s.setTier;
    else if (s.configured && s.detection.detected)
        tier = s.detection.tier;
    QualityResolver::setTier(tier);
    QualityResolver::setPlatformGroup(s.simulation.group ? *s.simulation.group : QualityResolver::platformGroupOf((int)Engine::getPlatform()));
}

DeviceTier Device::getTier(){
    const std::optional<DeviceTier> tier = QualityResolver::getTier();
    return tier ? *tier : deviceRunState().detection.tier;
}

bool Device::isTierApplied(){
    return QualityResolver::getTier().has_value();
}

void Device::setTier(DeviceTier tier){
    if (deviceRunState().setTier == tier)
        return;
    deviceRunState().setTier = tier;
    Log::print("Device tier: %s (Device.setTier or --device-tier)", DeviceNames::tierLabel(tier));
    applyTier();
}

void Device::resetTier(){
    deviceRunState().setTier.reset();
    applyTier();
}

bool Device::isTierSet(){
    return deviceRunState().setTier.has_value();
}

std::string Device::getGpuName(){
    return deviceRunState().detection.profile.gpuName;
}

// ---------------------------------------------------------------- signals

void Device::reportThermalState(ThermalState thermal){
    devicePlatformThermal.store((int)thermal, std::memory_order_relaxed);
}

void Device::reportLowMemory(){
    devicePendingLowMemory.fetch_add(1, std::memory_order_relaxed);
}

ThermalState Device::getThermalState(){
    return deviceRunState().thermal;
}

int Device::getLowMemoryCount(){
    return deviceRunState().lowMemoryCount;
}

void Device::update(double seconds){
    DeviceRunState& s = deviceRunState();

    const ThermalState platform = (ThermalState)devicePlatformThermal.load(std::memory_order_relaxed);
    const ThermalState thermal = s.simulation.thermal ? *s.simulation.thermal : platform;
    if (thermal != s.thermal){
        Log::print("Thermal state: %s (was %s)%s", DeviceNames::thermalLabel(thermal), DeviceNames::thermalLabel(s.thermal),
                   s.simulation.thermal ? ", simulated" : "");
        s.thermal = thermal;
        QualityResolver::setThermalState(thermal);
        onThermalStateChanged.call((int)thermal);
    }

    if (devicePendingLowMemory.exchange(0, std::memory_order_relaxed) > 0){
        s.lowMemoryCount++;
        QualityResolver::addMemoryPressure();
        // the CPU copies the pools can load again, and the pools' unused entries
        Engine::clearUnusedPools();
        Log::warn("Low memory (warning %d): cleared the unused pool entries; streamed textures keep %d more mip(s) off",
                  s.lowMemoryCount, QualityResolver::getMemoryDrop());
        onLowMemory.call();
    }

    QualityResolver::update(seconds);
    const ThermalState response = QualityResolver::getThermalResponse();
    if (response != s.response){
        s.response = response;
        logThermalResponse(response);
    }
}

// ---------------------------------------------------------------- the editor

void Device::setSimulation(const DeviceSimulation& simulation){
    if (simulation == deviceRunState().simulation)
        return;
    deviceRunState().simulation = simulation;
    applyTier();
}

const DeviceSimulation& Device::getSimulation(){
    return deviceRunState().simulation;
}

void Device::resetSession(){
    DeviceRunState& s = deviceRunState();
    s.setTier.reset();
    s.simulation = DeviceSimulation();
    s.thermal = (ThermalState)devicePlatformThermal.load(std::memory_order_relaxed);
    s.response = ThermalState::Nominal;
    s.lowMemoryCount = 0;
    devicePendingLowMemory.store(0, std::memory_order_relaxed);
    QualityResolver::resetResponse();
    QualityResolver::setThermalState(s.thermal);
    applyTier();
}
