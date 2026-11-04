// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Build Settings for games of every platform and the Publish tab (docs/specs/S30-shipping.md
// R29): the build method and why, the player runtime (installed, or Download with progress and
// cancel), Android's package format, keystore and tools, iOS's Mac requirement; then
// publishing.json's targets and the release steps, each a cancellable job that logs to the
// Console.

#include "ExportWindow.h"

#include "AppSettings.h"
#include "Backend.h"
#include "Generator.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "platform/AndroidPackager.h"
#include "platform/AndroidTools.h"
#include "platform/ExternalTools.h"
#include "platform/IosProject.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace jasperengine::editor {

namespace {

    void beginRow(const char* label) {
        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight());
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
    }

    const ImVec4 okColor(0.3f, 1.0f, 0.3f, 1.0f);
    const ImVec4 warningColor(1.0f, 0.5f, 0.0f, 1.0f);

    std::string megabytes(uint64_t bytes) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.0f MB", double(bytes) / (1024.0 * 1024.0));
        return text;
    }

    const char* backendLabel(const std::string& backend) {
        if (backend == "glcore") return "OpenGL";
        if (backend == "gles3") return "OpenGL ES 3 / WebGL 2";
        if (backend == "vulkan") return "Vulkan";
        if (backend == "d3d11") return "Direct3D 11";
        if (backend == "metal") return "Metal";
        return backend.c_str();
    }

}

ExportWindow::~ExportWindow() {
    finishBackgroundWork();
}

void ExportWindow::finishBackgroundWork() {
    if (m_download) {
        m_download->cancel.store(true);
        if (m_download->thread.joinable()) m_download->thread.join();
    }
    if (m_publishJob) {
        m_publishJob->cancel.store(true);
        if (m_publishJob->thread.joinable()) m_publishJob->thread.join();
    }
}

ExportWindow::ShipPlatform ExportWindow::shipPlatform() const {
    switch (m_platform) {
        case BuildPlatform::Windows: return ShipPlatform::Windows;
        case BuildPlatform::MacOS: return ShipPlatform::MacOS;
        case BuildPlatform::Android: return ShipPlatform::Android;
        case BuildPlatform::IOS: return ShipPlatform::IOS;
        case BuildPlatform::Web: return ShipPlatform::Web;
        default: return ShipPlatform::Linux;
    }
}

std::string ExportWindow::playerBackend() const {
    const std::vector<std::string> backends = jasperengine::editor::platformBackends(shipPlatform());
    auto it = m_playerBackendIndex.find(static_cast<int>(m_platform));
    const int index = it == m_playerBackendIndex.end() ? 0 : std::clamp(it->second, 0, static_cast<int>(backends.size()) - 1);
    return backends[static_cast<size_t>(index)];
}

void ExportWindow::refreshPlayerState() {
    if (!m_project) return;
    ExportConfig config;
    config.mode = ExportMode::Player;
    config.platform = shipPlatform();
    config.graphicBackend = playerBackend();
    parseBuildMethod(m_project->getBuildMethod(), config.method);
    m_playerRuntimeId = Exporter::playerIdFor(config, m_project->isDevelopmentBuild());
    m_playerRuntime = PlayerRuntimes::find(m_playerRuntimeId, {}, false, m_playerRuntimeError);
    m_methodChoice = Exporter::chooseBuildMethod(m_project, config, m_playerRuntime ? &*m_playerRuntime : nullptr);
    m_androidMissing.clear();
    m_androidLibrarySizes.clear();
    if (config.platform == ShipPlatform::Android && m_playerRuntime)
        m_androidLibrarySizes = platform::androidLibrarySizes(m_playerRuntime->dir / "android",
                                                              platform::selectedAndroidAbis(m_project->getAndroidProjectSettings()));
    if (config.platform == ShipPlatform::Android && m_methodChoice.method == BuildMethod::Prebuilt) {
        // (the bundletool jar is not among them: the build itself downloads it)
        m_androidMissing = platform::AndroidTools::discover(m_project->getAndroidProjectSettings().targetSdk).missing;
    }
    if (config.platform == ShipPlatform::IOS) m_iosXcodeMissing = platform::findXcode().missing;
}

bool ExportWindow::playerBuildBlocked(std::string& why) const {
    const ShipPlatform platform = shipPlatform();
    if (m_methodChoice.method == BuildMethod::Prebuilt) {
        if (!m_playerRuntime) {
            // Auto compiles instead when this computer can (Exporter::runPlayerBuild)
            const bool canCompile = platform == ShipPlatform::Web ? m_emsdkInfo.found
                                    : platform == hostBuildPlatform() && m_missingBuildTools.empty();
            const BuildMethod chosen = [this] {
                BuildMethod method = BuildMethod::Auto;
                parseBuildMethod(m_project->getBuildMethod(), method);
                return method;
            }();
            if (chosen == BuildMethod::Auto && canCompile) return false;
            why = "The " + m_playerRuntimeId + " player runtime is not installed: Download it above.";
            return true;
        }
        if (platform == ShipPlatform::Android && !m_androidMissing.empty()) {
            why = "Android tools are missing (Preferences > External Tools):";
            for (const std::string& item : m_androidMissing) why += "\n  - " + item;
            return true;
        }
        return false;
    }
    // Compile: this computer's platform and the web only
    if (platform == ShipPlatform::Web) {
        if (!m_emsdkInfo.found) {
            why = "Compiling for the web needs the Emscripten SDK (Preferences > External Tools).";
            return true;
        }
        return false;
    }
    if (platform != hostBuildPlatform()) {
        why = std::string("This project compiles from source (") + (m_methodChoice.reasons.empty() ? "Compile chosen" : m_methodChoice.reasons.front()) +
              "), which builds for this computer and the web only. Choose C++ Source Project and build it with " +
              (platform == ShipPlatform::Android ? "Android Studio" : platform == ShipPlatform::IOS ? "Xcode" : "that platform's CMake") + ".";
        return true;
    }
    if (!m_missingBuildTools.empty()) {
        why = "Compiling needs build tools:\n" + m_missingBuildTools;
        return true;
    }
    return false;
}

void ExportWindow::drawPlayerRows() {
    const ShipPlatform target = shipPlatform();

    // ---- graphics API
    beginRow("Graphics API");
    const std::vector<std::string> backends = jasperengine::editor::platformBackends(target);
    const std::string current = playerBackend();
    if (backends.size() == 1) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", backendLabel(current));
    } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##PlayerGraphicsApi", backendLabel(current))) {
            for (size_t i = 0; i < backends.size(); i++) {
                const bool selected = backends[i] == current;
                if (ImGui::Selectable(backendLabel(backends[i]), selected)) {
                    m_playerBackendIndex[static_cast<int>(m_platform)] = static_cast<int>(i);
                    saveCurrentSettingsToProject();
                    refreshPlayerState();
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // ---- build method: what Auto chose, and why
    beginRow("Build Method");
    static const char* const methodLabels[] = {"Auto", "Prebuilt", "Compile"};
    static const char* const methodNames[] = {"auto", "prebuilt", "compile"};
    int methodIndex = 0;
    for (int i = 0; i < 3; i++)
        if (m_project->getBuildMethod() == methodNames[i]) methodIndex = i;
    const float comboWidth = Theme::dpi(110.0f);
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::Combo("##BuildMethod", &methodIndex, methodLabels, 3)) {
        m_project->setBuildMethod(methodNames[methodIndex]);
        m_projectSettingsDirty = true;
        saveCurrentSettingsToProject();
        refreshPlayerState();
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (m_methodChoice.method == BuildMethod::Prebuilt) {
        ImGui::TextUnformatted("Prebuilt player (no compiler needed)");
    } else {
        const std::string reason = m_methodChoice.reasons.empty() ? std::string("Compile chosen") : m_methodChoice.reasons.front();
        ImGui::Text("Compile from source: %s", reason.c_str());
        if (m_methodChoice.reasons.size() > 1) {
            std::string all;
            for (const std::string& r : m_methodChoice.reasons) all += "- " + r + "\n";
            ImGui::SetItemTooltip("%s", all.c_str());
        }
    }

    // ---- the player runtime
    if (m_methodChoice.method == BuildMethod::Prebuilt) {
        beginRow("Player Runtime");
        drawRuntimeDownload();
    }

    // ---- platform requirements
    if (target == ShipPlatform::Android) {
        AndroidProjectSettings& android = m_project->getAndroidProjectSettings();
        beginRow("Package");
        // the App Bundle first (the default: Google Play serves each device its ABI), the
        // universal APK last and only when chosen
        static const AndroidFormat formats[] = {AndroidFormat::Aab, AndroidFormat::ApkPerAbi, AndroidFormat::Apk};
        static const char* const labels[] = {"App Bundle (AAB, Google Play)", "One APK per ABI", "Universal APK (every ABI)"};
        AndroidFormat current = AndroidFormat::Aab;
        parseAndroidFormat(android.packageFormat, current);
        int format = static_cast<int>(std::find(std::begin(formats), std::end(formats), current) - std::begin(formats));
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##AndroidPackage", &format, labels, IM_ARRAYSIZE(labels))) {
            android.packageFormat = androidFormatName(formats[std::clamp(format, 0, 2)]);
            current = formats[std::clamp(format, 0, 2)];
            m_projectSettingsDirty = true;
            saveCurrentSettingsToProject();
            refreshPlayerState();
        }
        // what a player download weighs: the engine's native libraries dominate it (content adds
        // its own); known once the runtime is installed
        if (!m_androidLibrarySizes.empty()) {
            auto mb = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
            uint64_t largest = 0, total = 0;
            std::string perAbi;
            for (const auto& [abi, bytes] : m_androidLibrarySizes) {
                largest = std::max(largest, bytes);
                total += bytes;
                char item[64];
                std::snprintf(item, sizeof(item), "%s%s %.1f MB", perAbi.empty() ? "" : ", ", abi.c_str(), mb(bytes));
                perAbi += item;
            }
            beginRow("Download Size");
            ImGui::AlignTextToFramePadding();
            if (current == AndroidFormat::Apk) {
                ImGui::TextColored(m_androidLibrarySizes.size() > 1 ? warningColor : okColor,
                                   "%.1f MB of engine libraries in one file (%s)", mb(total), perAbi.c_str());
                ImGui::SetItemTooltip("Every device downloads every selected ABI's libraries. App Bundle or One APK per ABI "
                                      "ship each device only its own (largest: %.1f MB).", mb(largest));
            } else {
                ImGui::TextColored(okColor, "up to %.1f MB of engine libraries per device (%s)", mb(largest), perAbi.c_str());
                ImGui::SetItemTooltip(current == AndroidFormat::Aab ? "Google Play serves each device the split of its ABI."
                                                                     : "One APK per ABI: give each device the one of its ABI.");
            }
        }
        beginRow("Keystore");
        ImGui::AlignTextToFramePadding();
        if (android.keystorePath.empty()) ImGui::TextDisabled("Debug keystore (testing only)");
        else ImGui::Text("%s (%s)", android.keystorePath.string().c_str(), android.keystoreAlias.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Keystore...")) {
            m_playerSettingsRequested = true;
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip("Project Settings > Android > Keystore; its passwords are asked when you build and never saved.");
        if (m_methodChoice.method == BuildMethod::Prebuilt) {
            beginRow("Tools");
            if (m_androidMissing.empty()) {
                ImGui::TextColored(okColor, ICON_FA_CIRCLE_CHECK " JDK, build-tools and platform found (no NDK or Gradle needed)");
            } else {
                for (const std::string& missing : m_androidMissing) {
                    ImGui::TextColored(warningColor, ICON_FA_CIRCLE_XMARK " %s", missing.c_str());
                }
                ImGui::TextDisabled("Set paths in Preferences > External Tools (Android SDK, JDK).");
            }
        }
    } else if (target == ShipPlatform::IOS) {
        beginRow("Requires");
        ImGui::AlignTextToFramePadding();
        if (!m_iosXcodeMissing.empty())
            ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " Requires %s: this makes the Xcode project", m_iosXcodeMissing.c_str());
        else if (m_iosSimulator)
            ImGui::TextColored(okColor, ICON_FA_CIRCLE_CHECK " Xcode found: an unsigned .app for the simulator");
        else if (m_project->getIOSProjectSettings().teamId.empty())
            ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " No Team ID (Project Settings > iOS): compiled for a device, not signed");
        else
            ImGui::TextColored(okColor, ICON_FA_CIRCLE_CHECK " Xcode found: the build is signed and exported (Project Settings > iOS)");
        beginRow("Target");
        ImGui::Checkbox("Simulator##ios", &m_iosSimulator);
        ImGui::SetItemTooltip("An unsigned .app for the iOS Simulator (needs no Apple account); off: for a device.");
    } else if (target == ShipPlatform::MacOS) {
        beginRow("Signing");
        ImGui::AlignTextToFramePadding();
        if (platform::ExternalTools::codesign().found() || platform::ExternalTools::rcodesign().found())
            ImGui::TextDisabled("Signed ad hoc (codesign or rcodesign)");
        else
            ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " Unsigned: set rcodesign in Preferences > External Tools");
    }
}

void ExportWindow::drawRuntimeDownload() {
    if (m_download && m_download->running.load()) {
        const uint64_t received = m_download->received.load();
        const float fraction = m_download->size ? std::min(1.0f, float(double(received) / double(m_download->size))) : 0.0f;
        const std::string overlay = megabytes(received) + (m_download->size ? " / " + megabytes(m_download->size) : std::string());
        ImGui::ProgressBar(m_download->size ? fraction : -1.0f * float(ImGui::GetTime()), ImVec2(-Theme::dpi(80.0f), 0), overlay.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel##RuntimeDownload")) m_download->cancel.store(true);
        Backend::getApp().requestRedraw();
        return;
    }
    if (m_download && m_download->thread.joinable()) {
        m_download->thread.join();
        {
            std::lock_guard<std::mutex> lock(m_download->mutex);
            if (m_download->failed) Out::error("Player runtime %s: %s", m_download->id.c_str(), m_download->message.c_str());
            else Out::info("Installed player runtime %s", m_download->id.c_str());
        }
        refreshPlayerState();
    }
    ImGui::AlignTextToFramePadding();
    if (m_playerRuntime) {
        ImGui::TextColored(okColor, ICON_FA_CIRCLE_CHECK " %s installed (%s)", m_playerRuntimeId.c_str(), megabytes(m_playerRuntime->sizeBytes).c_str());
        return;
    }
    if (PlayerRuntimes::isReleaseEditor()) {
        const uint64_t size = PlayerRuntimes::archiveSize(m_playerRuntimeId);
        const std::string label = "Download (" + megabytes(size) + ")##RuntimeDownload";
        if (ImGui::Button(label.c_str())) startRuntimeDownload(m_playerRuntimeId);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m_playerRuntimeId.c_str());
    } else {
        ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " %s not installed", m_playerRuntimeId.c_str());
        ImGui::SetItemTooltip("A development editor uses local runtimes: tools/build.sh players, then JASPER_PLAYERS_DIR "
                              "(or Preferences > External Tools > Install from file...).");
    }
    if (m_download) {
        std::lock_guard<std::mutex> lock(m_download->mutex);
        if (m_download->failed) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(warningColor, "%s", m_download->message.c_str());
            ImGui::PopTextWrapPos();
        }
    }
}

void ExportWindow::startRuntimeDownload(const std::string& id) {
    if (m_download && m_download->thread.joinable()) m_download->thread.join();
    m_download = std::make_unique<RuntimeDownload>();
    m_download->id = id;
    m_download->size = PlayerRuntimes::archiveSize(id);
    m_download->running.store(true);
    RuntimeDownload* download = m_download.get();
    download->thread = std::thread([download]() {
        std::string installed, error;
        std::vector<std::string> warnings;
        const bool ok = PlayerRuntimes::download(download->id, {}, &download->cancel, installed, warnings, error, &download->received);
        {
            std::lock_guard<std::mutex> lock(download->mutex);
            download->failed = !ok;
            download->message = ok ? installed : error;
        }
        for (const std::string& warning : warnings) Out::warning("%s", warning.c_str());
        download->running.store(false);
    });
}

void ExportWindow::drawKeystorePasswordDialog() {
    if (m_askKeystorePasswords) {
        ImGui::OpenPopup("Keystore Passwords##ExportKeystore");
        m_askKeystorePasswords = false;
    }
    ImGui::SetNextWindowSizeConstraints(Theme::dpi(ImVec2(420.0f, 0.0f)), ImVec2(Theme::dpi(420.0f), FLT_MAX));
    if (!ImGui::BeginPopupModal("Keystore Passwords##ExportKeystore", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        return;
    const AndroidProjectSettings& android = m_project->getAndroidProjectSettings();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::Text("Signing with %s (%s). The passwords are used for this build only and never saved.",
                android.keystorePath.string().c_str(), android.keystoreAlias.c_str());
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##KeystorePassword", "Keystore password", m_keystorePassword, sizeof(m_keystorePassword), ImGuiInputTextFlags_Password);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##KeyPassword", "Key password (empty: the keystore's)", m_keyPassword, sizeof(m_keyPassword), ImGuiInputTextFlags_Password);
    ImGui::Spacing();
    const float width = Theme::dpi(120.0f);
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - width * 2.0f - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(m_keystorePassword[0] == '\0');
    if (ImGui::Button("Build", ImVec2(width, 0))) {
        ImGui::CloseCurrentPopup();
        startConfiguredExport(m_pendingOverwrite);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(width, 0))) {
        std::fill(std::begin(m_keystorePassword), std::end(m_keystorePassword), '\0');
        std::fill(std::begin(m_keyPassword), std::end(m_keyPassword), '\0');
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- Publish tab

void ExportWindow::loadPublishing() {
    m_publishingLoaded = true;
    m_publishingDirty = false;
    m_publishingError.clear();
    const fs::path file = publish::PublishingConfig::pathOf(m_project->getProjectPath());
    std::error_code ec;
    m_publishingExists = fs::is_regular_file(file, ec);
    if (!m_publishingExists) {
        m_publishing = publish::PublishingConfig();
        return;
    }
    if (!publish::PublishingConfig::load(file, m_publishing, m_publishingError)) m_publishing = publish::PublishingConfig();
    m_deckTarget.clear();
    for (const auto& [name, target] : m_publishing.targets)
        if (target.deviceProfile == "steamdeck") m_deckTarget = name;
}

bool ExportWindow::savePublishing() {
    std::string error;
    if (!m_publishing.save(publish::PublishingConfig::pathOf(m_project->getProjectPath()), error)) {
        m_publishingError = error;
        Out::error("publishing.json: %s", error.c_str());
        return false;
    }
    m_publishingExists = true;
    m_publishingDirty = false;
    m_publishingError.clear();
    return true;
}

void ExportWindow::startPublishJob(const std::string& title, std::function<bool(const publish::PublishTools&, std::string&)> work) {
    if (m_publishJob && m_publishJob->running.load()) return;
    if (m_publishJob && m_publishJob->thread.joinable()) m_publishJob->thread.join();
    m_publishJob = std::make_unique<PublishJob>();
    m_publishJob->title = title;
    m_publishJob->running.store(true);
    PublishJob* job = m_publishJob.get();
    Out::info("Publish: %s...", title.c_str());
    job->thread = std::thread([job, work = std::move(work)]() {
        publish::PublishTools tools;
        tools.log = [](const std::string& line) { Out::info("%s", line.c_str()); };
        tools.cancelled = [job]() { return job->cancel.load(); };
        std::string message;
        const bool ok = work(tools, message);
        {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->failed = !ok;
            job->message = message;
            job->done = true;
        }
        if (ok) Out::info("Publish: %s: %s", job->title.c_str(), message.c_str());
        else Out::error("Publish: %s failed: %s", job->title.c_str(), message.c_str());
        job->running.store(false);
    });
}

void ExportWindow::drawPublishTargets() {
    static const char* const platforms[] = {"windows", "linux", "macos", "android", "ios", "web"};
    std::string removeTarget;
    if (ImGui::BeginTable("##PublishTargets", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                                     ImGuiTableFlags_ScrollY, ImVec2(0, Theme::dpi(170.0f)))) {
        for (const char* column : {"Target", "Platform", "Arch", "API", "Flavour", "Build Folder", "Executable", "Depot", ""})
            ImGui::TableSetupColumn(column);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (auto& [name, target] : m_publishing.targets) {
            ImGui::PushID(name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(name.c_str());
            if (target.deviceProfile == "steamdeck") ImGui::SetItemTooltip("Steam Deck target");
            ImGui::TableNextColumn();
            int platform = 1;
            for (int i = 0; i < 6; i++)
                if (target.platform == platforms[i]) platform = i;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##platform", &platform, platforms, 6)) {
                target.platform = platforms[platform];
                ShipPlatform parsed = ShipPlatform::Linux;
                parseBuildPlatform(target.platform, parsed);
                target.arch = platformArch(parsed);
                target.backend = jasperengine::editor::platformBackends(parsed).front();
                m_publishingDirty = true;
            }
            auto text = [this](const char* id, std::string& value) {
                char buffer[512];
                std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText(id, buffer, sizeof(buffer))) {
                    value = buffer;
                    m_publishingDirty = true;
                }
            };
            ImGui::TableNextColumn();
            text("##arch", target.arch);
            ImGui::TableNextColumn();
            text("##backend", target.backend);
            ImGui::TableNextColumn();
            bool development = target.flavour == "development";
            if (ImGui::Checkbox("Dev##flavour", &development)) {
                target.flavour = development ? "development" : "release";
                m_publishingDirty = true;
            }
            ImGui::TableNextColumn();
            text("##buildDir", target.buildDir);
            ImGui::TableNextColumn();
            text("##executable", target.executable);
            ImGui::TableNextColumn();
            text("##depot", target.depotId);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(ICON_FA_TRASH "##remove")) removeTarget = name;
            ImGui::SetItemTooltip("Remove this target");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!removeTarget.empty()) {
        m_publishing.targets.erase(removeTarget);
        m_publishingDirty = true;
    }
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    ImGui::InputTextWithHint("##NewTarget", "new target name", m_newTargetName, sizeof(m_newTargetName));
    ImGui::SameLine();
    ImGui::BeginDisabled(!publish::isIdentifier(m_newTargetName) || m_publishing.targets.count(m_newTargetName));
    if (ImGui::Button("Add Target")) {
        publish::PublishTarget target;
        // this platform's build folder as Build Settings remembers it
        const ShipPlatform platform = shipPlatform();
        target.platform = buildPlatformName(platform);
        target.arch = platformArch(platform);
        target.backend = playerBackend();
        target.flavour = m_project->isDevelopmentBuild() ? "development" : "release";
        target.buildDir = m_targetDir.generic_string();
        m_publishing.targets[m_newTargetName] = target;
        m_newTargetName[0] = '\0';
        m_publishingDirty = true;
    }
    ImGui::EndDisabled();
}

void ExportWindow::drawPublishActions() {
    const bool running = m_publishJob && m_publishJob->running.load();
    const fs::path projectDir = m_project->getProjectPath();
    const publish::PublishingConfig config = m_publishing;
    const std::string release = m_releaseName;
    const fs::path releasesRoot = fs::path(config.releasesDir).is_absolute() ? fs::path(config.releasesDir) : projectDir / config.releasesDir;
    const fs::path releaseDir = releasesRoot / release;

    ImGui::BeginDisabled(running || m_publishingDirty);
    ImGui::SetNextItemWidth(Theme::dpi(140.0f));
    ImGui::InputTextWithHint("##ReleaseName", "release, e.g. 1.0.0", m_releaseName, sizeof(m_releaseName));
    ImGui::SameLine();
    const bool named = publish::isIdentifier(release);
    ImGui::BeginDisabled(!named);
    if (ImGui::Button("Prepare")) {
        publish::PrepareRequest request;
        request.projectDir = projectDir;
        request.projectName = m_project->getName();
        request.config = config;
        request.release = release;
        startPublishJob("Prepare " + release, [request](const publish::PublishTools& tools, std::string& message) {
            publish::PrepareResult result;
            std::vector<std::string> problems;
            if (!publish::prepareRelease(request, tools, result, problems)) {
                for (const std::string& problem : problems) message += (message.empty() ? "" : "\n") + problem;
                return false;
            }
            for (const std::string& warning : result.warnings) Out::warning("%s", warning.c_str());
            message = "prepared " + result.releaseDir.string();
            return true;
        });
    }
    ImGui::SetItemTooltip("Validate every target's build, copy it into the release, split the debug symbols, write release.json");
    ImGui::SameLine();
    if (ImGui::Button("Verify")) {
        startPublishJob("Verify " + release, [releaseDir](const publish::PublishTools&, std::string& message) {
            std::vector<std::string> problems;
            publish::ReleaseInfo info;
            if (!publish::verifyRelease(releaseDir, problems, &info)) {
                for (const std::string& problem : problems) message += (message.empty() ? "" : "\n") + problem;
                return false;
            }
            message = "unchanged since prepare";
            for (const auto& [name, target] : info.targets)
                if (!target.glibcFloor.empty()) message += "; " + name + " needs GLIBC_" + target.glibcFloor;
            return true;
        });
    }
    ImGui::EndDisabled();

    // ---- Steam
    ImGui::Spacing();
    ImGui::TextUnformatted("Steam");
    ImGui::SameLine();
    ImGui::Checkbox("Preview", &m_steamPreview);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    ImGui::InputTextWithHint("##SteamBranch", "beta branch", m_steamBranch, sizeof(m_steamBranch));
    ImGui::SameLine();
    const std::string branch = m_steamBranch;
    const bool preview = m_steamPreview;
    ImGui::BeginDisabled(!named);
    if (ImGui::Button("Steam VDF")) {
        startPublishJob("SteamPipe scripts of " + release, [releaseDir, preview, branch](const publish::PublishTools&, std::string& message) {
            fs::path app;
            std::vector<fs::path> depots;
            if (!publish::writeSteamScripts(releaseDir, preview, branch, app, depots, message)) return false;
            message = "wrote " + app.string();
            return true;
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Upload...")) m_confirmUpload = true;
    ImGui::EndDisabled();

    // ---- Steam Deck
    ImGui::Spacing();
    ImGui::TextUnformatted("Steam Deck");
    ImGui::SameLine();
    auto deckText = [this](const char* id, const char* hint, std::string& value, float width) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        ImGui::SetNextItemWidth(Theme::dpi(width));
        if (ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer))) {
            value = buffer;
            m_publishingDirty = true;
        }
        ImGui::SameLine();
    };
    ImGui::EndDisabled();
    ImGui::BeginDisabled(running);
    deckText("##DeckHost", "host", m_publishing.deck.host, 120.0f);
    deckText("##DeckUser", "user", m_publishing.deck.user, 70.0f);
    deckText("##DeckDir", "remote dir", m_publishing.deck.remoteDir, 140.0f);
    ImGui::SetNextItemWidth(Theme::dpi(100.0f));
    if (ImGui::BeginCombo("##DeckTarget", m_deckTarget.empty() ? "target" : m_deckTarget.c_str())) {
        for (const auto& [name, target] : m_publishing.targets) {
            if (target.platform != "linux") continue;
            if (ImGui::Selectable(name.c_str(), name == m_deckTarget)) m_deckTarget = name;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(running || m_publishingDirty);
    const publish::DeckSettings deck = m_publishing.deck;
    const std::string deckTarget = m_deckTarget;
    ImGui::BeginDisabled(!named || deckTarget.empty() || deck.host.empty());
    if (ImGui::Button("Deploy to Deck")) {
        startPublishJob("Deploy " + release + " to " + deck.host, [releaseDir, deckTarget, deck](const publish::PublishTools& tools, std::string& message) {
            std::string output;
            const bool ok = publish::deckDeploy(releaseDir, deckTarget, deck, tools, output, message);
            if (ok) message = output.empty() ? "deployed" : output;
            return ok;
        });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(deck.host.empty());
    if (ImGui::Button("Rollback")) {
        startPublishJob("Rollback on " + deck.host, [deck](const publish::PublishTools& tools, std::string& message) {
            std::string output;
            const bool ok = publish::deckRollback(deck, tools, output, message);
            if (ok) message = output;
            return ok;
        });
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // ---- the running job
    if (m_publishJob) {
        ImGui::Spacing();
        if (m_publishJob->running.load()) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s...", m_publishJob->title.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel##PublishJob")) m_publishJob->cancel.store(true);
            Backend::getApp().requestRedraw();
        } else {
            std::lock_guard<std::mutex> lock(m_publishJob->mutex);
            ImGui::PushTextWrapPos(0.0f);
            if (m_publishJob->failed) ImGui::TextColored(warningColor, ICON_FA_CIRCLE_XMARK " %s: %s", m_publishJob->title.c_str(), m_publishJob->message.c_str());
            else ImGui::TextColored(okColor, ICON_FA_CIRCLE_CHECK " %s: %s", m_publishJob->title.c_str(), m_publishJob->message.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // ---- Upload: a person's step, confirmed
    if (m_confirmUpload) {
        ImGui::OpenPopup("Upload to Steam##PublishUpload");
        m_confirmUpload = false;
    }
    if (ImGui::BeginPopupModal("Upload to Steam##PublishUpload", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(Theme::dpi(420.0f));
        ImGui::Text("Upload release %s to Steam app %s%s%s? SteamCMD logs in with its own saved session (log in once in a terminal: "
                    "steamcmd +login <account>).", release.c_str(), config.steam.appId.c_str(), branch.empty() ? "" : ", live on ",
                    branch.c_str());
        ImGui::PopTextWrapPos();
        if (m_steamUser[0] == '\0') {
            if (const char* user = std::getenv("STEAM_BUILD_USER")) std::snprintf(m_steamUser, sizeof(m_steamUser), "%s", user);
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##SteamUser", "Steam build account", m_steamUser, sizeof(m_steamUser));
        const std::string user = m_steamUser;
        ImGui::BeginDisabled(user.empty());
        if (ImGui::Button("Upload", ImVec2(Theme::dpi(120.0f), 0))) {
            const fs::path steamcmd = platform::ExternalTools::steamcmd().path;
            startPublishJob("Upload " + release + " to Steam", [releaseDir, user, branch, steamcmd](const publish::PublishTools& tools, std::string& message) {
                if (!publish::steamUpload(releaseDir, user, branch, steamcmd, tools, message)) return false;
                message = "uploaded";
                return true;
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(Theme::dpi(120.0f), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void ExportWindow::drawPublishTab() {
    if (!m_publishingLoaded) loadPublishing();
    if (m_publishJob && !m_publishJob->running.load() && m_publishJob->thread.joinable()) m_publishJob->thread.join();

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("publishing.json names the builds of a release. Prepare validates and copies them, Verify checks nothing changed, "
                        "then Steam and the Steam Deck take the release (docs/shipping.md).");
    ImGui::PopTextWrapPos();
    if (!m_publishingError.empty()) ImGui::TextColored(warningColor, ICON_FA_TRIANGLE_EXCLAMATION " %s", m_publishingError.c_str());

    if (!m_publishingExists && m_publishing.targets.empty()) {
        if (ImGui::Button("Create publishing.json")) {
            m_publishing = publish::defaultPublishingConfig({{buildPlatformName(shipPlatform()), m_targetDir.generic_string()}});
            savePublishing();
        }
        return;
    }

    ImGui::Spacing();
    drawPublishTargets();

    ImGui::Spacing();
    char appId[32];
    std::snprintf(appId, sizeof(appId), "%s", m_publishing.steam.appId.c_str());
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    if (ImGui::InputTextWithHint("##SteamAppId", "Steam app id", appId, sizeof(appId), ImGuiInputTextFlags_CharsDecimal)) {
        m_publishing.steam.appId = appId;
        m_publishingDirty = true;
    }
    ImGui::SameLine();
    char releases[512];
    std::snprintf(releases, sizeof(releases), "%s", m_publishing.releasesDir.c_str());
    ImGui::SetNextItemWidth(Theme::dpi(200.0f));
    if (ImGui::InputTextWithHint("##ReleasesDir", "releases folder", releases, sizeof(releases))) {
        m_publishing.releasesDir = releases;
        m_publishingDirty = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_publishingDirty);
    if (ImGui::Button("Save")) {
        const std::vector<std::string> problems = m_publishing.check();
        if (problems.empty()) savePublishing();
        else m_publishingError = problems.front();
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) loadPublishing();
    ImGui::EndDisabled();
    if (m_publishingDirty) {
        ImGui::SameLine();
        ImGui::TextDisabled("(save before the release steps)");
    }

    ImGui::Separator();
    drawPublishActions();
}

}
