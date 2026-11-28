// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtExportKeys.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleTranslations.h"
#include "titleart/TitleArtRenderHost.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/render/TitleGpu.h"
#include "asset/AssetFileWrite.h"
#include "titleanim/TitleAnimTypes.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // title_fx's features, in the order the shader declares them (TitleArtSystem's LiveFeature)
    enum TitleArtExportFeature : uint32_t {
        EXPORT_FX_SWEEP = 1u << 0,
        EXPORT_FX_GLOW = 1u << 1,
        EXPORT_FX_TINT = 1u << 2,
        EXPORT_FX_GLINTS = 1u << 3,
        EXPORT_FX_CROSSFADE = 1u << 4,
        EXPORT_FX_ANIM = 1u << 5
    };
    const char* const EXPORT_FX_NAMES[] = {"SWEEP", "GLOW", "TINT", "GLINTS", "CROSSFADE", "ANIM"};

    // whether the document has animations, and the Live Effects their live.* tracks turn on
    bool titleArtExportAnimationEffects(const fs::path& projectRoot, const std::string& titlePath, uint32_t& effects){
        effects = 0;
        if (projectRoot.empty() || titlePath.empty()) return false;
        std::string text;
        if (!assetio::readFile(projectRoot / fs::path(titlePath), text)) return false;
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        if (!parsed.ok) return false;
        const std::shared_ptr<titleanim::TitleAnimLibrary> library = titleart::compileAnimations(parsed.doc, nullptr, nullptr);
        if (!library || library->animations.empty()) return false;
        for (const titleanim::CompiledTitleAnimation& animation : library->animations){
            for (const titleanim::CompiledTitleTrack& track : animation.tracks){
                if (track.target != titleanim::TrackTarget::Title || track.mute || track.orphan) continue;
                switch (track.titleField){
                    case titleanim::TitleField::SweepPosition:
                    case titleanim::TitleField::SweepAngle:
                    case titleanim::TitleField::SweepWidth: effects |= EXPORT_FX_SWEEP; break;
                    case titleanim::TitleField::GlowPulse: effects |= EXPORT_FX_GLOW; break;
                    case titleanim::TitleField::TintColor:
                    case titleanim::TitleField::TintAmount: effects |= EXPORT_FX_TINT; break;
                    case titleanim::TitleField::GlintAmount: effects |= EXPORT_FX_GLINTS; break;
                    default: break;
                }
            }
        }
        return true;
    }

}

std::vector<std::vector<std::string>> editor::titleArtLiveFeatureSets(const TitleArtRendererComponent& title, const fs::path& projectRoot){
    uint32_t base = 0;
    if (title.sweepEnabled) base |= EXPORT_FX_SWEEP;
    if (title.glowEnabled) base |= EXPORT_FX_GLOW;
    if (title.tintEnabled) base |= EXPORT_FX_TINT;
    if (title.glintsEnabled) base |= EXPORT_FX_GLINTS;
    std::set<uint32_t> sets;
    if (base) sets.insert(base);
    // S85 R13: a playing animation draws with ANIM, and its live.* tracks turn their effects on
    // (animations play together as overlays: every combination of what they turn on)
    uint32_t animated = 0;
    if (titleArtExportAnimationEffects(projectRoot, title.title.path, animated)){
        for (uint32_t subset = animated;; subset = (subset - 1) & animated){
            const uint32_t playing = base | subset | EXPORT_FX_ANIM;
            sets.insert(playing);
            // R16: a Baked flipbook frame has no glow output, so Glow is off over it
            if (title.renderMode == TitleArtRenderMode::BAKED && (playing & EXPORT_FX_GLOW)) sets.insert(playing & ~(uint32_t)EXPORT_FX_GLOW);
            if (subset == 0) break;
        }
    }
    // a Realtime title that crossfades draws the crossfade while a new image comes in
    if (title.renderMode != TitleArtRenderMode::BAKED && title.transition == TitleArtTransition::CROSSFADE){
        std::set<uint32_t> fading = sets;
        fading.insert(base);
        for (uint32_t features : fading) sets.insert(features | EXPORT_FX_CROSSFADE);
    }
    std::vector<std::vector<std::string>> out;
    for (uint32_t features : sets){
        std::vector<std::string> names;
        for (size_t f = 0; f < 6; f++) if (features & (1u << f)) names.push_back(EXPORT_FX_NAMES[f]);
        out.push_back(std::move(names));
    }
    return out;
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
    // Lua can select layers hidden in the source language. An unnamed source is not in the codes.
    std::vector<std::string> languages = titleart::titleLanguageCodes(doc->root);
    if (titleart::titleSourceLanguage(doc->root).empty()) languages.emplace_back();
    for (int tier : tiers){
        for (uint32_t flags : {0u, titleart::TITLE_ART_TEST_NO_FLOAT_TARGETS}){
            for (titleart::OutputContent content : {titleart::OutputContent::All, titleart::OutputContent::Glow}){
                for (const std::string& language : languages){
                    titleart::RenderRequest request;
                    request.doc = doc;
                    request.testFlags = flags;
                    request.content = content;
                    request.language = language;
                    request.tier = tier;
                    request.assets = &resolver;
                    std::unique_ptr<titleart::TitleRenderJob> job = renderer.begin(request);
                    if (!job) continue;
                    for (const ShaderKey& key : job->shaderKeys()) keys.insert(key);
                }
            }
        }
    }
    // a Glyph Atlas title composes its cells (R28): its glyph cells and plates use the programs above
    keys.insert(titleart::titleRenderProgramKey((uint32_t)titleart::TitleRenderProgram::GlyphCompose));
    keys.insert(titleart::titleRenderProgramKey((uint32_t)titleart::TitleRenderProgram::GlyphComposeAnim));
    return true;
}
