// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtExportKeys.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleGpu.h"
#include "asset/AssetFileWrite.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

std::vector<std::vector<std::string>> editor::titleArtLiveFeatureSets(const TitleArtRendererComponent& title){
    std::vector<std::string> base;
    if (title.sweepEnabled) base.push_back("SWEEP");
    if (title.glowEnabled) base.push_back("GLOW");
    if (title.tintEnabled) base.push_back("TINT");
    if (title.glintsEnabled) base.push_back("GLINTS");
    std::vector<std::vector<std::string>> sets;
    if (!base.empty()) sets.push_back(base);
    // a Realtime title that crossfades draws the crossfade while a new image comes in
    if (title.renderMode != TitleArtRenderMode::BAKED && title.transition == TitleArtTransition::CROSSFADE){
        std::vector<std::string> fading = base;
        fading.push_back("CROSSFADE");
        sets.push_back(fading);
    }
    return sets;
}

bool editor::addTitleArtRenderKeys(const fs::path& projectRoot, const std::string& titlePath, std::set<ShaderKey>& keys, std::string* error){
    std::string text;
    if (!assetio::readFile(projectRoot / fs::path(titlePath), text)){
        if (error) *error = "cannot read " + titlePath;
        return false;
    }
    titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
    if (!parsed.ok){
        if (error) *error = titlePath + ": " + parsed.error.toString();
        return false;
    }
    auto doc = std::make_shared<const titleart::TitleArtDoc>(std::move(parsed.doc));
    // the job's plan names every program it binds; nothing runs (no step is stepped)
    titleart::TitleArtRenderer renderer;
    EditorTitleAssetResolver resolver(nullptr, projectRoot, projectRoot / "assets");
    // S86 §6.4: a 3D title's variants per Realtime tier (the tier resolves on the device)
    bool has3D = false;
    titleart::forEachLayer(doc->root, [&has3D](const titleart::Json& layer, const titleart::Json*, int){
        has3D = has3D || layer.value("dimension", std::string()) == "3d";
        return !has3D;
    });
    const std::vector<int> tiers = has3D ? std::vector<int>{1, 2, 3} : std::vector<int>{0};
    for (int tier : tiers){
        for (uint32_t flags : {0u, titleart::TITLE_ART_TEST_NO_FLOAT_TARGETS}){
            for (titleart::OutputContent content : {titleart::OutputContent::All, titleart::OutputContent::Glow}){
                titleart::RenderRequest request;
                request.doc = doc;
                request.testFlags = flags;
                request.content = content;
                request.tier = tier;
                request.assets = &resolver;
                std::unique_ptr<titleart::TitleRenderJob> job = renderer.begin(request);
                if (!job) continue;
                for (const ShaderKey& key : job->shaderKeys()) keys.insert(key);
            }
        }
    }
    // a Glyph Atlas title composes its cells (R28): its glyph cells and plates use the programs above
    keys.insert(titleart::titleRenderProgramKey((uint32_t)titleart::TitleRenderProgram::GlyphCompose));
    return true;
}
