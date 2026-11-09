// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/PixelArtMode.h"

#include "Out.h"
#include "Project.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/SetAssetMetaCmd.h"
#include "command/type/SetMainCameraCmd.h"
#include "pixelart/PixelArtInput.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureImportSettings.h"
#include "window/ConsoleWindow.h"

#include "component/PixelPerfectCameraComponent.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pixelart;

namespace {
    constexpr const char* kSummaryPrefix = "Pixel Art Check:";
    // the debounce of an import batch, and the longest wait for new textures' import settings
    constexpr std::chrono::milliseconds kSettle(1000);
    constexpr std::chrono::milliseconds kSettingsWait(10000);
}

PixelArtMode& PixelArtMode::get(){
    static PixelArtMode mode;
    return mode;
}

void PixelArtMode::projectSettingsChanged(Project* project){
    const PixelArtSettings& settings = project->getPixelArtSettings();
    importHook = settings.enabled && settings.checkOnImport;
    playHook = settings.enabled && settings.checkOnPlay;
    owner = project;
    if (!importHook){
        pendingTextures.clear();
        pending = false;
    }
    if (!playHook)
        pendingPlay = false;
    // the Console's click on a summary exists only while something can log one
    if (importHook || playHook){
        ConsoleWindow::setMessageAction(kSummaryPrefix, [this](const std::string&){
            if (openAction && !lastKey.empty())
                openAction(lastKey);
        });
    }else{
        ConsoleWindow::setMessageAction(kSummaryPrefix, nullptr);
    }
}

void PixelArtMode::assetsChanged(Project* project, const std::vector<std::string>& textures){
    if (!importHook || project != owner)
        return;
    for (const std::string& texture : textures)
        pendingTextures.insert(texture);
    if (!pending)
        pendingSince = std::chrono::steady_clock::now();
    pending = true;
    lastChange = std::chrono::steady_clock::now();
}

void PixelArtMode::playStarting(Project* project){
    if (!playHook || project != owner)
        return;
    // the check of the scenes as Play found them: started now, logged when it finishes
    if (running.isValid() && !running.isReady()){
        pendingPlay = true;
        return;
    }
    start(project, "Play");
}

void PixelArtMode::start(Project* project, const std::string& reason){
    std::vector<PixelProblem> errors;
    auto input = std::make_shared<PixelArtCheckInput>(inputFromProject(*project, Scope(), errors));
    runningReason = reason;
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.name = "pixel-art-check";
    running = jobs::submit([input](){ return std::make_shared<CheckResult>(run(*input)); }, options);
}

void PixelArtMode::tick(Project* project){
    if ((!importHook && !playHook) || project != owner)
        return;
    if (running.isValid() && running.isReady()){
        std::shared_ptr<CheckResult> result;
        try{
            result = running.get();
        }catch (...){
            result.reset();
        }
        running = jobs::Task<std::shared_ptr<CheckResult>>();
        if (result){
            const std::string line = summaryLine(*result);
            if (!line.empty()){
                for (const Finding& finding : result->findings){
                    if (finding.status != RuleStatus::Pass){
                        lastKey = finding.key;
                        break;
                    }
                }
                Out::warning("%s", line.c_str());
            }
        }
    }
    if (running.isValid())
        return;
    if (pendingPlay){
        pendingPlay = false;
        start(project, "Play");
        return;
    }
    if (!pending)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastChange < kSettle)
        return;
    // new textures get their import settings from the cook queue's inference: the check waits
    // for them (or gives up waiting), so it judges what they import as
    bool waiting = false;
    for (const std::string& texture : pendingTextures){
        const std::filesystem::path absolute = project->getProjectPath() / texture;
        std::error_code ec;
        if (!std::filesystem::exists(absolute, ec) || !std::filesystem::exists(SetAssetMetaCmd::metaPathOf(absolute), ec))
            continue;
        const YAML::Node stored = SetAssetMetaCmd::read(absolute, TextureImportSettings::metaKey);
        if (!stored || stored.IsNull())
            waiting = true;
    }
    if (waiting && now - pendingSince < kSettingsWait)
        return;
    pending = false;
    pendingTextures.clear();
    start(project, "import");
}

std::string PixelArtMode::summaryLine(const CheckResult& result){
    const int fails = result.count(RuleStatus::Fail);
    const int warns = result.count(RuleStatus::Warn);
    if (fails == 0 && warns == 0)
        return std::string();
    std::string line = kSummaryPrefix;
    if (fails > 0)
        line += " " + std::to_string(fails) + " FAIL" + (warns > 0 ? "," : "");
    if (warns > 0)
        line += " " + std::to_string(warns) + " WARN";
    for (const Finding& finding : result.findings){
        if (finding.status != RuleStatus::Pass){
            line += " - " + finding.message;
            break;
        }
    }
    return line;
}

void PixelArtMode::setupNewScene(Project& project, uint32_t sceneId){
    SceneProject* scene = project.getScene(sceneId);
    if (!scene || !scene->scene)
        return;
    CreateEntityCmd creator(&project, sceneId, "Main Camera", EntityCreationType::CAMERA);
    creator.setQuiet(true);
    if (!creator.execute())
        return;
    const Entity camera = creator.getEntity();
    if (camera == NULL_ENTITY)
        return;
    SetMainCameraCmd mainCamera(&project, sceneId, camera);
    mainCamera.execute();
    AddComponentCmd component(&project, sceneId, camera, ComponentType::PixelPerfectCameraComponent);
    if (component.execute()){
        if (PixelPerfectCameraComponent* ppc = scene->scene->findComponent<PixelPerfectCameraComponent>(camera))
            ppc->upscaleMode = project.getPixelArtSettings().upscaleMode;
    }
}
