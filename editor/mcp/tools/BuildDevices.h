// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The device part of the build tools (docs/specs/build-and-run-on-device.md R21-R24):
// get_build_info devices, build_project run_device / patch, run_build device. The runners are
// editor/platform/DeviceRunner.h's, shared with Build Settings and the CLI.

#include "Exporter.h"
#include "mcp/McpCall.h"
#include "platform/DeviceRunner.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::mcp::builddevice {

    // a build's run on a device: what was decided before the build, and how the deploy went
    struct BuildDeploy {
        platform::RunDevice device;
        bool patch = false;                 // the build cooks content only and pushes it
        std::string fullBecause;            // a patch was asked for and cannot be: why it installs in full
        std::string androidAbi;             // Android: the device's ABI the APK holds
        std::string playerId;
        std::string playerIdentity;
        bool headless = false;
        platform::DeployResult result;
    };

    // The run on `device` of a build of `config` (Build Settings' and resolve's): the APK's ABI, the
    // player identity and patch or full install (R6); nullptr with the error naming the fix
    std::shared_ptr<BuildDeploy> prepare(Project* project, const ExportConfig& config, const platform::RunDevice& device,
                                         std::optional<bool> patch, std::string& error);

    // Probes the devices of the build's platform off the main thread, picks `runDevice` among them
    // and decides patch or full install (R6); then(deploy) on the main thread, or the call fails
    // naming the fix. `patch`: the call's (default: true for a Development Build).
    void resolve(const CallPtr& call, Project* project, const ExportConfig& config, const std::string& runDevice, std::optional<bool> patch,
                 std::function<void(std::shared_ptr<BuildDeploy>)> then);

    // The build's watcher thread, once the export succeeded: records the package (R15), installs or
    // patches, launches. False when the deploy failed (deploy.result.error says why).
    bool deploy(BuildDeploy& deploy, const std::filesystem::path& projectPath, const ExportResult& result, const ExportConfig& config,
                const std::function<void(const std::string&)>& progress);
    // the result's device {...} and the next hint
    Json resultJson(const BuildDeploy& deploy);
    std::string nextHint(const BuildDeploy& deploy);

    // get_build_info devices:true: probes every platform's devices off the main thread (a result
    // younger than 2 s reused), then replies with data.devices
    void replyWithDevices(const CallPtr& call, Project* project, const std::string& text, Json data);

    // run_build device: the build recorded for the device's platform, installed or patched when the
    // device runs another, launched with the run's options, watched, captured and stopped
    void runBuildOnDevice(const CallPtr& call);

}
