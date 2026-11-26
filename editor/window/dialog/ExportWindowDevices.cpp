// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Build Settings' Run Device row, Build And Run on a device and Patch And Run
// (docs/specs/build-and-run-on-device.md R18): the devices of the selected platform, probed off the
// UI thread (when Android or iOS shows, on Refresh, every 3 s while it stays shown); the choice
// is per user (.jasperengine/user/devices.yaml), so it has no undo, as the selected platform. A
// finished build runs on the device through the shared device runner, its log in the Console.

#include "ExportWindow.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "mcp/tools/BuildDevices.h"
#include "platform/DeviceRecords.h"
#include "platform/DeviceRunner.h"
#include "thread/ThreadUtil.h"
#include "util/UIProbe.h"
#include "window/EditorModal.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>

namespace jasperengine::editor {

namespace {

    double devicesNow() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool runsOnDevices(BuildPlatform platform) {
        return platform == BuildPlatform::Android || platform == BuildPlatform::IOS || platform == BuildPlatform::Web;
    }

    std::string deviceLabel(const platform::RunDevice& device) {
        return device.name + " (" + device.id + ") \xE2\x80\x94 " + device.state;
    }

}

ExportWindow::ShipPlatform ExportWindow::runPlatform() const {
    if (m_profiles.hasSelection() && m_project) {
        const BuildProfileEntry* profile = buildprofiles::find(buildprofiles::view(m_project->getProjectPath()).entries, m_profiles.selectedName());
        if (profile && profile->readable) return profile->profile.platform;
    }
    return shipPlatform();
}

void ExportWindow::pollRunDevices(bool force) {
    const ShipPlatform platform = runPlatform();
    if (!m_selectedDevicesLoaded && m_project) {
        m_selectedDevices = platform::DeviceRecords::load(m_project->getProjectInternalPath()).selected;
        m_selectedDevicesLoaded = true;
    }
    // a probe that ended: its list
    if (m_deviceProbe && m_deviceProbe->done.load()) {
        if (m_deviceProbe->thread.joinable()) m_deviceProbe->thread.join();
        if (m_deviceProbe->platform == platform) {
            m_runDevices = std::move(m_deviceProbe->devices);
            m_runDevicesPlatform = platform;
            m_patchStateDirty = true;
        }
        m_deviceProbe.reset();
    }
    if (m_runDevicesPlatform != platform) {
        m_runDevices.clear();
        m_runDevicesPlatform = platform;
        m_patchStateDirty = true;
        force = true;
    }
    // no process while nothing needs one: the browser and this computer are known without a probe
    if (platform != ShipPlatform::Android && platform != ShipPlatform::IOS) {
        m_runDevices = platform::probeDevices(platform, 0);
        return;
    }
    const double now = devicesNow();
    if (m_deviceProbe || (!force && m_lastDeviceProbe >= 0.0 && now - m_lastDeviceProbe < 3.0)) return;
    m_lastDeviceProbe = now;
    m_deviceProbe = std::make_unique<DeviceProbe>();
    m_deviceProbe->platform = platform;
    const unsigned int minSdk = m_project ? m_project->getAndroidProjectSettings().minSdk : 0;
    DeviceProbe* probe = m_deviceProbe.get();
    probe->thread = jasperengine::thread::spawnNamed("je-device-probe", [probe, platform, minSdk]() {
        probe->devices = platform::probeDevices(platform, minSdk);
        probe->done = true;
    });
}

const platform::RunDevice* ExportWindow::selectedRunDevice() const {
    const ShipPlatform platform = runPlatform();
    if (!runsOnDevices(platform)) return nullptr;
    const auto it = m_selectedDevices.find(buildPlatformName(platform));
    const platform::RunDevice* first = nullptr;
    for (const platform::RunDevice& device : m_runDevices) {
        if (it != m_selectedDevices.end() && device.id == it->second) return &device;
        if (!first && device.usable) first = &device;
    }
    return first;
}

bool ExportWindow::runDeviceUsable(std::string& why) const {
    const ShipPlatform platform = runPlatform();
    if (!runsOnDevices(platform)) {
        if (platform == hostBuildPlatform()) return true;
        why = "Build And Run needs a game for this computer, an Android device, an iOS Simulator or the browser.";
        return false;
    }
    const platform::RunDevice* device = selectedRunDevice();
    if (device && device->usable) return true;
    if (device) {
        why = device->name + ": " + device->why;
    } else if (platform == ShipPlatform::Android) {
        why = m_runDevices.size() == 1 && m_runDevices.front().id == "adb" ? m_runDevices.front().why
                                                                            : "No Android device: connect one with USB debugging on, then Refresh";
    } else if (platform == ShipPlatform::IOS) {
        why = m_runDevices.empty() ? "No iOS Simulator: Refresh" : m_runDevices.front().why;
    } else {
        why = "No device to run on";
    }
    return false;
}

void ExportWindow::refreshPatchState() {
    m_patchStateDirty = false;
    m_patchAvailable = false;
    m_patchWhyNot.clear();
    const ShipPlatform platform = runPlatform();
    if (platform != ShipPlatform::Android && platform != ShipPlatform::IOS) return;
    const platform::RunDevice* device = selectedRunDevice();
    if (!device || !device->usable) {
        std::string why;
        runDeviceUsable(why);
        m_patchWhyNot = why;
        return;
    }
    ExportConfig config;
    config.platform = platform;
    config.graphicBackend = m_profiles.hasSelection() ? std::string() : playerBackend();
    config.developmentBuild = m_project->isDevelopmentBuild();
    std::string error;
    const std::shared_ptr<mcp::builddevice::BuildDeploy> deploy = mcp::builddevice::prepare(m_project, config, *device, true, error);
    if (!deploy) {
        m_patchWhyNot = error;
        return;
    }
    m_patchAvailable = deploy->patch;
    m_patchWhyNot = deploy->fullBecause;
}

void ExportWindow::drawRunDeviceRow() {
    const ShipPlatform platform = runPlatform();
    ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Run Device");
    ImGui::TableNextColumn();
    if (!runsOnDevices(platform)) {
        ImGui::AlignTextToFramePadding();
        if (platform == hostBuildPlatform()) ImGui::TextDisabled("This computer");
        else ImGui::TextDisabled("Runs on %s, not here", buildPlatformName(platform));
        return;
    }
    pollRunDevices(false);
    if (m_patchStateDirty) refreshPatchState();

    std::vector<std::string> labels;
    int index = -1;
    const platform::RunDevice* selected = selectedRunDevice();
    for (size_t i = 0; i < m_runDevices.size(); i++) {
        labels.push_back(deviceLabel(m_runDevices[i]));
        if (selected && &m_runDevices[i] == selected) index = static_cast<int>(i);
    }
    auto choose = [&](int chosen) {
        if (chosen < 0 || chosen >= static_cast<int>(m_runDevices.size())) return;
        m_selectedDevices[buildPlatformName(platform)] = m_runDevices[static_cast<size_t>(chosen)].id;
        platform::DeviceRecords records = platform::DeviceRecords::load(m_project->getProjectInternalPath());
        records.selected = m_selectedDevices;
        std::string error;
        if (!records.save(m_project->getProjectInternalPath(), error)) Out::warning("%s", error.c_str());
        m_patchStateDirty = true;
    };
    int agentIndex = index;
    if (EditorModal::enumField("run_device", agentIndex, labels)) choose(agentIndex);

    const float refreshWidth = ImGui::CalcTextSize("Refresh").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(Theme::dpi(80.0f), ImGui::GetContentRegionAvail().x - refreshWidth - ImGui::GetStyle().ItemSpacing.x));
    const std::string preview = index >= 0 ? labels[static_cast<size_t>(index)]
                              : m_deviceProbe ? std::string("Looking for devices...") : std::string("No device");
    if (ImGui::BeginCombo("##RunDevice", preview.c_str())) {
        for (size_t i = 0; i < m_runDevices.size(); i++) {
            const platform::RunDevice& device = m_runDevices[i];
            ImGui::BeginDisabled(!device.usable);
            if (ImGui::Selectable(labels[i].c_str(), static_cast<int>(i) == index)) choose(static_cast<int>(i));
            ImGui::EndDisabled();
            if (!device.usable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", device.why.c_str());
        }
        ImGui::EndCombo();
    }
    UIProbe::item("build_settings.run_device");
    if (selected && !selected->usable) ImGui::SetItemTooltip("%s", selected->why.c_str());
    ImGui::SameLine();
    if (EditorModal::button("Refresh", "refresh_devices")) pollRunDevices(true);
    ImGui::SetItemTooltip("Look for devices again (adb devices, simctl list); Build Settings looks every 3 s while it shows them.");
    if (platform == ShipPlatform::IOS) {
        ImGui::TextDisabled("Physical iPhones: build the .ipa and install it with Xcode");
    }
}

void ExportWindow::applyRunDevice(ExportConfig& config, bool patch) {
    m_patchRun = patch;
    if (!m_runAfterBuild && !patch) return;
    const platform::RunDevice* device = selectedRunDevice();
    if (!device) return;
    if (patch) {
        config.contentOnly = true;
        return;
    }
    if (config.platform == ShipPlatform::Android) {
        std::string error;
        const std::shared_ptr<mcp::builddevice::BuildDeploy> deploy = mcp::builddevice::prepare(m_project, config, *device, false, error);
        if (deploy) config.androidRunAbi = deploy->androidAbi;
        else Out::warning("Build And Run: %s", error.c_str());
    }
    // a simulator runs the simulator .app (R3)
    if (config.platform == ShipPlatform::IOS) config.iosSimulator = true;
}

void ExportWindow::startDeviceRun() {
    const ShipPlatform platform = m_lastExportConfig.platform;
    platform::RunDevice device = platform::hostDevice();
    if (runsOnDevices(platform)) {
        const platform::RunDevice* chosen = selectedRunDevice();
        if (!chosen) {
            Out::error("Build And Run: no Run Device for %s (Build Settings > Run Device)", buildPlatformName(platform));
            return;
        }
        device = *chosen;
    }
    std::string error;
    std::shared_ptr<mcp::builddevice::BuildDeploy> deploy = mcp::builddevice::prepare(m_project, m_lastExportConfig, device, m_patchRun, error);
    if (!deploy) {
        Out::error("Build And Run: %s", error.c_str());
        return;
    }
    if (m_patchRun && !deploy->patch) {
        Out::error("Patch And Run on %s: %s; use Build And Run", device.name.c_str(), deploy->fullBecause.c_str());
        return;
    }
    ExportResult result = m_exporter.getResult();
    // a Desktop (compiled) build has no player report: its executable
    if (result.executable.empty()) result.executable = builtPlayerPath();
    const ExportConfig config = m_lastExportConfig;
    const fs::path projectPath = m_project->getProjectPath();
    if (m_deviceRunThread.joinable()) m_deviceRunThread.join();
    Out::info("%s on %s...", m_patchRun ? "Patch And Run" : "Build And Run", device.name.c_str());
    // installing takes seconds: off the UI thread, its phases in the Console's device strip
    m_deviceRunThread = jasperengine::thread::spawnNamed("je-device-run", [deploy, projectPath, result, config]() {
        if (!mcp::builddevice::deploy(*deploy, projectPath, result, config, {})) {
            Out::error("Build And Run on %s: %s", deploy->device.name.c_str(), deploy->result.error.c_str());
            return;
        }
        if (deploy->result.installed == "patched")
            Out::info("Patched %s: %zu files (no reinstall), launched", deploy->device.name.c_str(), deploy->result.pushedFiles);
        else
            Out::info("Installed on %s and launched%s%s", deploy->device.name.c_str(), deploy->result.url.empty() ? "" : ": ",
                      deploy->result.url.c_str());
    });
}

}
