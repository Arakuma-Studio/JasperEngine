// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtRuntimeExport.h"

#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "component/RectTransformComponent.h"
#include "players/ModuleUsage.h"
#include "subsystem/TitleArtRuntime.h"
#include "subsystem/TitleArtSystem.h"
#include "texsynth/SynthImage.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/runtime/TitleArtLive.h"

#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // R11's warning threshold (§9): more distinct initial states than this still prebake
    constexpr size_t PREBAKE_COUNT_WARNING = 64;

}

TitleArtExportPlan editor::planTitleArtExport(Project* project){
    TitleArtExportPlan plan;
    if (!project) return plan;
    const ModuleScan scan = ModuleUsageDetector::collect(project);
    std::set<std::string> runtime;
    std::set<std::string> all;
    for (const ModuleContent& content : scan.contents){
        EntityRegistry* registry = content.registry;
        if (!registry || !registry->isComponentRegistered<TitleArtRendererComponent>()) continue;
        for (Entity entity : content.entities){
            if (!registry->isEntityCreated(entity)) continue;
            const TitleArtRendererComponent* title = registry->findComponent<TitleArtRendererComponent>(entity);
            if (!title || title->title.path.empty()) continue;
            all.insert(title->title.path);
            if (!title->enabled) continue;
            TitleArtShippedInstance instance;
            instance.where = content.label;
            instance.object = registry->getEntityName(entity);
            instance.title = title->title.path;
            instance.mode = title->renderMode;
            instance.overrides = title->overrides;
            instance.additionalCharacters = title->additionalCharacters;
            instance.prebake = title->prebakeInitialState;
            instance.glow = title->glowEnabled;
            TitleArtBakeInfo bake;
            std::string error;
            if (TitleArtBakeInfo::load(titleArtSidecarPath(title->title.path), bake, error)) instance.region = bake.frame;
            const bool ui = TitleArtSystem::spaceOf(registry, entity) == titleart::TitleArtSpace::UI;
            const RectTransformComponent* rect = ui ? registry->findComponent<RectTransformComponent>(entity) : nullptr;
            instance.resolution = TitleArtSystem::resolutionFor(bake, ui, rect ? rect->width : 0u, title->uiScale, title->resolutionScale);
            if (title->renderMode != TitleArtRenderMode::BAKED) runtime.insert(title->title.path);
            plan.instances.push_back(std::move(instance));
        }
    }
    plan.runtimeTitles = runtime;
    for (const std::string& title : all)
        if (!runtime.count(title)) plan.bakedTitles.insert(title);
    return plan;
}

TitleArtPrebakeResult editor::writeTitleArtPrebakes(Project* project, const TitleArtExportPlan& plan, const fs::path& assetsDst){
    TitleArtPrebakeResult result;
    if (!project) return result;
    const fs::path assetsRelative = project->getAssetsPath().lexically_relative(project->getProjectPath());
    const std::string assetsPrefix = assetsRelative.empty() || assetsRelative == "." ? std::string() : assetsRelative.generic_string() + "/";
    std::map<uint64_t, std::string> written;    // key -> file (dedup by state, R11)
    for (const TitleArtShippedInstance& instance : plan.instances){
        if (instance.mode == TitleArtRenderMode::BAKED || !instance.prebake) continue;
        std::string error;
        // the runtime's own key: a live state of the document, never stepped
        std::shared_ptr<titleart::TitleArtLive> live = titleart::TitleArtRuntime::create(instance.title, instance.mode, &error);
        if (!live){
            result.warnings.push_back("Title Art " + instance.object + " (" + instance.where + "): no prebaked image: " + error);
            continue;
        }
        titleart::TitleArtLiveState state;
        state.overrides = instance.overrides;
        state.resolution = instance.resolution;
        state.region = instance.region;
        state.glowOutput = instance.glow;
        const uint64_t key = titleart::TitleArtRuntime::prebakeKeyOf(*live, state);
        if (written.count(key)) continue;
        const std::string file = titleArtPrebakePath(instance.title, key);

        titleart::RenderRequest request;
        request.doc = live->document();
        request.overrides = titleart::titleArtFieldOverrides(*live->document(), instance.overrides);
        request.pxScale = instance.resolution;
        request.region = {instance.region.x, instance.region.y, instance.region.width, instance.region.height};
        request.output = titleart::TitleOutputFormat::Srgb8;
        TitleRenderedImage image;
        if (!TitleArtRenderHost::editor().render(project, request, image, error)){
            result.warnings.push_back("Title Art " + instance.object + " (" + instance.where + "): the initial state did not render (" + error +
                                      "): the game renders it at its first frame");
            continue;
        }
        std::vector<uint8_t> png;
        if (!texsynth::encodePng(image.image, png)){
            result.ok = false;
            result.error = "cannot encode the prebaked image of " + instance.title;
            return result;
        }
        // the build's assets mirror the project's below the assets directory
        std::string inAssets = file;
        if (!assetsPrefix.empty() && inAssets.compare(0, assetsPrefix.size(), assetsPrefix) == 0) inAssets = inAssets.substr(assetsPrefix.size());
        const fs::path target = assetsDst / fs::path(inAssets);
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (!assetio::writeFile(target, std::string_view(reinterpret_cast<const char*>(png.data()), png.size()), assetio::Durability::Cache, &error)){
            result.ok = false;
            result.error = "cannot write " + target.string() + ": " + error;
            return result;
        }
        written[key] = file;
        result.written.push_back(file);
        result.bytes += png.size();
    }
    if (written.size() > PREBAKE_COUNT_WARNING){
        result.warnings.push_back("W-TITLEART-PREBAKE-COUNT: " + std::to_string(written.size()) + " distinct initial states of Realtime titles were "
                                  "prebaked (" + std::to_string(result.bytes / 1024) + " KiB): fewer distinct overrides or sizes, or Prebake Initial "
                                  "State off on some, make the build smaller");
    }
    return result;
}
