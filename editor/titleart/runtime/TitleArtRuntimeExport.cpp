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
#include "titleart/TitleArtHash.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleGlyphAtlas.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"
#include "titleart/three_d/Title3DExport.h"

#include <fstream>
#include <iterator>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // R11's warning threshold (§9): more distinct initial states than this still prebake
    constexpr size_t PREBAKE_COUNT_WARNING = 64;
    // R32: the cells a title's prebaked atlas holds at most
    constexpr size_t GLYPH_PREBAKE_CELLS = 1024;

    // the build's copy of a document (what the player reads and hashes: its fonts relinked to subsets)
    std::shared_ptr<const titleart::TitleArtDoc> shippedDocument(const fs::path& path){
        std::ifstream in(path, std::ios::binary);
        if (!in) return nullptr;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        if (!parsed.ok) return nullptr;
        return std::make_shared<const titleart::TitleArtDoc>(std::move(parsed.doc));
    }

}

std::string editor::titleArtRuntimeSettingsText(const TitleArtProjectSettings& settings){
    auto group = [](const TitleArtGroupValues& v){ return titleart::Json{{"desktop", v.desktop}, {"mobile", v.mobile}, {"web", v.web}}; };
    const titleart::Json root = {{"version", 1}, {"frame_budget_ms", group(settings.frameBudgetMs)}, {"release_delay_s", group(settings.releaseDelaySeconds)},
                                 {"cache_budget_mib", group(settings.cacheBudgetMiB)}, {"reference_height", settings.referenceHeight}};
    return root.dump(1);
}

bool editor::writeTitleArtRuntimeSettings(Project* project, const TitleArtExportPlan& plan, const fs::path& assetsDst, std::string& error){
    if (!project || plan.runtimeTitles.empty()) return true;
    const fs::path target = assetsDst / titleart::TITLE_ART_RUNTIME_SETTINGS_PATH;
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    return assetio::writeFile(target, titleArtRuntimeSettingsText(project->getTitleArtSettings()), assetio::Durability::Cache, &error);
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
            instance.atlasPageSize = title->atlasPageSize;
            instance.atlasMaxPages = title->atlasMaxPages;
            instance.qualityTier = title->qualityTier;
            instance.runtimeEnvironments = title->runtimeEnvironments;
            TitleArtBakeInfo bake;
            std::string error;
            if (TitleArtBakeInfo::load(titleArtSidecarPath(title->title.path), bake, error)) instance.region = bake.frame;
            const bool ui = TitleArtSystem::spaceOf(registry, entity) == titleart::TitleArtSpace::UI;
            const RectTransformComponent* rect = ui ? registry->findComponent<RectTransformComponent>(entity) : nullptr;
            // R11: as the game computes it, from the project's Reference Height and canvas
            const int referenceHeight = project->getTitleArtSettings().referenceHeight;
            const float referenceScale = referenceHeight > 0 && project->getCanvasHeight() > 0 ? (float)referenceHeight / (float)project->getCanvasHeight() : 1.0f;
            instance.resolution = TitleArtSystem::resolutionFor(bake, ui, rect ? rect->width : 0u, title->uiScale, title->resolutionScale, referenceScale);
            if (title->renderMode != TitleArtRenderMode::BAKED) runtime.insert(title->title.path);
            plan.instances.push_back(std::move(instance));
        }
    }
    plan.runtimeTitles = runtime;
    for (const std::string& title : all)
        if (!runtime.count(title)) plan.bakedTitles.insert(title);
    return plan;
}

// R32: a Glyph Atlas instance's atlas, rendered here and shipped beside the title
void editor::writeGlyphPrebake(Project* project, const TitleArtShippedInstance& instance, std::shared_ptr<const titleart::TitleArtDoc> doc,
                               uint64_t shippedHash, const titleart::TitleArtLiveState& state, const fs::path& assetsDst,
                               const std::string& assetsPrefix, TitleArtPrebakeResult& result){
    const titleart::TitleGlyphAtlasPlan plan = titleart::planTitleGlyphAtlas(*doc);
    if (!plan.fallback.empty() || plan.textLayer.empty()) return;
    const std::string who = "Title Art " + instance.object + " (" + instance.where + ")";
    // the project's document renders; the build's copy's hash keys the style, as the player's
    titleart::TitleArtLive live(instance.title, TitleArtRenderMode::GLYPH_ATLAS, doc, shippedHash);
    live.setGlyphPlan(plan);
    titleart::TitleArtRenderer renderer;
    const titleart::TitleJobFactory factory = titleart::makeRendererJobFactory(renderer, TitleArtRenderHost::editor().resolver(project));
    const std::vector<titleart::FieldOverride> overrides = titleart::titleArtFieldOverrides(*doc, instance.overrides);
    std::string initial;
    for (const titleart::FieldOverride& o : overrides)
        if (o.layerId == plan.textLayer && o.path == "text" && o.value.is_string()) initial = o.value.get<std::string>();
    if (initial.empty()){
        if (const titleart::Json* layer = titleart::findLayer(doc->root, plan.textLayer)) initial = layer->value("text", std::string());
    }
    std::string error;
    if (!titleart::prebakeTitleGlyphAtlas(live, factory, state, overrides, plan.textLayer, titleart::titleGlyphPrebakeStrings(initial),
                                          GLYPH_PREBAKE_CELLS, error))
        result.warnings.push_back(who + ": the prebaked glyph atlas stops at " + error);
    const std::shared_ptr<titleart::TitleGlyphAtlas>& atlas = live.glyphAtlas();
    if (!atlas || atlas->cellCount() == 0) return;
    titleart::TitleGlyphAtlasSnapshot snapshot;
    if (!atlas->snapshot(snapshot, error)){
        result.warnings.push_back(who + ": no prebaked glyph atlas: " + error);
        return;
    }
    std::string manifest = titleArtGlyphPrebakePath(instance.title, atlas->stats().styleHash);
    if (!assetsPrefix.empty() && manifest.compare(0, assetsPrefix.size(), assetsPrefix) == 0) manifest = manifest.substr(assetsPrefix.size());
    if (!titleart::TitleGlyphAtlas::writePrebaked((assetsDst / manifest).string(), snapshot, result.bytes, error)){
        result.warnings.push_back(who + ": " + error);
        return;
    }
    result.written.push_back(manifest);
    result.glyphCells += atlas->cellCount();
}

TitleArtPrebakeResult editor::writeTitleArtPrebakes(Project* project, const TitleArtExportPlan& plan, const fs::path& assetsDst,
                                                   const std::vector<std::string>& platforms){
    TitleArtPrebakeResult result;
    if (!project) return result;
    const fs::path assetsRelative = project->getAssetsPath().lexically_relative(project->getProjectPath());
    const std::string assetsPrefix = assetsRelative.empty() || assetsRelative == "." ? std::string() : assetsRelative.generic_string() + "/";
    std::map<uint64_t, std::string> written;    // key -> file (dedup by state, R11)
    for (const TitleArtShippedInstance& instance : plan.instances){
        if (instance.mode == TitleArtRenderMode::BAKED || !instance.prebake) continue;
        std::string error;
        // the project's document renders (its fonts); the build's copy keys (the player hashes it)
        std::shared_ptr<titleart::TitleArtLive> live = titleart::TitleArtRuntime::create(instance.title, instance.mode, &error);
        if (!live){
            result.warnings.push_back("Title Art " + instance.object + " (" + instance.where + "): no prebaked image: " + error);
            continue;
        }
        std::string inAssetsTitle = instance.title;
        if (!assetsPrefix.empty() && inAssetsTitle.compare(0, assetsPrefix.size(), assetsPrefix) == 0) inAssetsTitle = inAssetsTitle.substr(assetsPrefix.size());
        std::shared_ptr<const titleart::TitleArtDoc> shipped = shippedDocument(assetsDst / fs::path(inAssetsTitle));
        if (!shipped) shipped = live->document();
        const uint64_t shippedHash = titleart::hashTitleArt(*shipped).document;
        titleart::TitleArtLive keyLive(instance.title, instance.mode, shipped, shippedHash);
        titleart::TitleArtLiveState state;
        state.overrides = instance.overrides;
        state.resolution = instance.resolution;
        state.region = instance.region;
        state.glowOutput = instance.glow;
        state.atlasPageSize = instance.atlasPageSize;
        state.atlasMaxPages = instance.atlasMaxPages;
        state.label = "TitleArt/" + instance.object;
        if (instance.mode == TitleArtRenderMode::GLYPH_ATLAS)
            writeGlyphPrebake(project, instance, live->document(), shippedHash, state, assetsDst, assetsPrefix, result);
        // S86 R37: a 3D title's state holds the tier it renders at on the device
        for (const int tier : titleArt3DPrebakeTiers(project, *live->document(), instance.qualityTier, platforms)){
            state.tier = tier;
            const uint64_t key = titleart::TitleArtRuntime::prebakeKeyOf(keyLive, state);
            if (written.count(key)) continue;
            const std::string file = titleArtPrebakePath(instance.title, key);

            titleart::RenderRequest request;
            request.doc = live->document();
            request.overrides = titleart::titleArtFieldOverrides(*live->document(), instance.overrides);
            request.pxScale = instance.resolution;
            request.region = {instance.region.x, instance.region.y, instance.region.width, instance.region.height};
            request.output = titleart::TitleOutputFormat::Srgb8;
            request.tier = tier;
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
    }
    if (written.size() > PREBAKE_COUNT_WARNING){
        result.warnings.push_back("W-TITLEART-PREBAKE-COUNT: " + std::to_string(written.size()) + " distinct initial states of Realtime titles were "
                                  "prebaked (" + std::to_string(result.bytes / 1024) + " KiB): fewer distinct overrides or sizes, or Prebake Initial "
                                  "State off on some, make the build smaller");
    }
    return result;
}
