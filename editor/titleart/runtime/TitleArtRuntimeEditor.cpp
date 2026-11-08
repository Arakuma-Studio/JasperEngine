// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtRuntimeEditor.h"

#include "Project.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/runtime/TitleArtScheduler.h"

#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    titleart::TitleArtRuntimeSettings runtimeSettings(Project* project){
        const TitleArtProjectSettings& p = project->getTitleArtSettings();
        titleart::TitleArtRuntimeSettings s;
        s.frameBudgetMs[(size_t)DevicePlatformGroup::Desktop] = p.frameBudgetMs.desktop;
        s.frameBudgetMs[(size_t)DevicePlatformGroup::Mobile] = p.frameBudgetMs.mobile;
        s.frameBudgetMs[(size_t)DevicePlatformGroup::Web] = p.frameBudgetMs.web;
        s.editorFrameBudgetMs = p.editorFrameBudgetMs;
        s.releaseDelaySeconds[(size_t)DevicePlatformGroup::Desktop] = p.releaseDelaySeconds.desktop;
        s.releaseDelaySeconds[(size_t)DevicePlatformGroup::Mobile] = p.releaseDelaySeconds.mobile;
        s.releaseDelaySeconds[(size_t)DevicePlatformGroup::Web] = p.releaseDelaySeconds.web;
        s.cacheBudgetMiB[(size_t)DevicePlatformGroup::Desktop] = p.cacheBudgetMiB.desktop;
        s.cacheBudgetMiB[(size_t)DevicePlatformGroup::Mobile] = p.cacheBudgetMiB.mobile;
        s.cacheBudgetMiB[(size_t)DevicePlatformGroup::Web] = p.cacheBudgetMiB.web;
        s.referenceHeight = p.referenceHeight;
        s.simulateExportCharacters = p.simulateExportCharacters;
        const std::filesystem::path assets = project->getAssetsPath().lexically_relative(project->getProjectPath());
        s.assetsPrefix = assets.empty() || assets == "." ? std::string() : assets.generic_string() + "/";
        return s;
    }

    bool sameSettings(const titleart::TitleArtRuntimeSettings& a, const titleart::TitleArtRuntimeSettings& b){
        for (size_t g = 0; g < DEVICE_PLATFORM_GROUP_COUNT; g++){
            if (a.frameBudgetMs[g] != b.frameBudgetMs[g] || a.releaseDelaySeconds[g] != b.releaseDelaySeconds[g] ||
                a.cacheBudgetMiB[g] != b.cacheBudgetMiB[g]) return false;
        }
        return a.editorFrameBudgetMs == b.editorFrameBudgetMs && a.referenceHeight == b.referenceHeight &&
               a.simulateExportCharacters == b.simulateExportCharacters && a.assetsPrefix == b.assetsPrefix;
    }

}

bool editor::syncTitleArtRuntime(Project* project){
    titleart::TitleArtScheduler& scheduler = titleart::TitleArtScheduler::get();
    static const titleart::AssetResolver* lastResolver = nullptr;
    if (!project || project->getProjectPath().empty()){
        if (lastResolver){
            scheduler.setAssetResolver(nullptr);
            lastResolver = nullptr;
        }
        return false;
    }
    // the project's resolver (made again by the host when the project changes); the host owns it
    const titleart::AssetResolver* resolver = TitleArtRenderHost::editor().resolver(project);
    if (resolver != lastResolver){
        scheduler.setAssetResolver(std::shared_ptr<const titleart::AssetResolver>(resolver, [](const titleart::AssetResolver*){}));
        lastResolver = resolver;
    }
    const titleart::TitleArtRuntimeSettings settings = runtimeSettings(project);
    if (!sameSettings(settings, scheduler.settings())) scheduler.configure(settings);
    for (const titleart::TitleArtLive* live : scheduler.entries())
        if (live->hasWork()) return true;
    return false;
}
