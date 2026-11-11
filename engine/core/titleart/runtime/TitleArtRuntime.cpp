// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// TitleArtRuntime with the `titleart` module (docs/specs/S81-title-art-runtime.md R9-R24): the
// core's seam onto TitleArtLive and the engine's one TitleArtScheduler. A title's state turns
// into S80 field overrides through the document's Exposed parameters (R5), and into the state
// hash the scheduler, the cache and the export's prebake share (R9, R11).

#include "subsystem/TitleArtRuntime.h"

#include "Engine.h"
#include "device/QualityResolver.h"
#include "io/Data.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleTranslations.h"
#include "titleart/TitleArtIO.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/runtime/TitleLiveCompose.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/anim/TitleAnimRealtime.h"
#include "titleart/text/TitleTextLayout.h"
#include "titleanim/TitleAnimPlayer.h"
#include "util/Color.h"
#include "Log.h"
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
#include "titleart/three_d/TitleCooked3D.h"
#include "titleart/three_d/TitleTierFeatures.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    bool readText(const std::string& path, std::string& text){
        Data data;
        if (data.open(path.c_str()) != FILEDATA_OK) return false;
        text.assign(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
        return true;
    }

    std::string srgbHex(const Vector4& linear){
        const Vector4 srgb = Color::linearTosRGB(linear);
        TitleColor c{std::min(1.0f, std::max(0.0f, srgb.x)), std::min(1.0f, std::max(0.0f, srgb.y)), std::min(1.0f, std::max(0.0f, srgb.z))};
        return formatTitleColor(c);
    }

    // an override's value as the document stores the field (§6.1 value types)
    Json overrideJson(const TitleArtOverride& o){
        if (o.type == "number") return Json((double)o.value.x);
        if (o.type == "bool") return Json(o.value.x != 0.0f);
        if (o.type == "color") return Json(srgbHex(o.value));
        if (o.type == "font" || o.type == "image"){
            TitleAssetRef ref;
            ref.path = o.text;
            return encodeAssetRef(ref);
        }
        return Json(o.text);
    }

    // the document's Exposed parameter `name`, null when it has none
    const Json* exposedOf(const TitleArtDoc& doc, const std::string& name){
        auto it = doc.root.find("exposed");
        if (it == doc.root.end() || !it->is_array()) return nullptr;
        for (const Json& item : *it)
            if (item.is_object() && item.value("name", std::string()) == name) return &item;
        return nullptr;
    }

    std::vector<FieldOverride> fieldOverrides(const TitleArtDoc& doc, const std::vector<TitleArtOverride>& overrides){
        return titleArtFieldOverrides(doc, overrides);
    }

}

std::vector<FieldOverride> jasperengine::titleart::titleArtFieldOverrides(const TitleArtDoc& doc, const std::vector<TitleArtOverride>& overrides){
    std::vector<FieldOverride> out;
    for (const TitleArtOverride& o : overrides){
        const Json* exposed = exposedOf(doc, o.name);
        if (!exposed) continue;
        const Json value = overrideJson(o);
        for (const Json& target : exposed->value("targets", Json::array())){
            if (!target.is_object()) continue;
            out.push_back(FieldOverride{target.value("layer", std::string()), target.value("field", std::string()), value});
        }
    }
    return out;
}

namespace {

    // the field's value as an override holds it (R5's document value)
    bool fieldValue(const Json& value, const std::string& type, TitleArtOverride& out){
        if (type == "number"){
            if (!value.is_number()) return false;
            out.value = Vector4((float)value.get<double>(), 0.0f, 0.0f, 0.0f);
            return true;
        }
        if (type == "bool"){
            if (!value.is_boolean()) return false;
            out.value = Vector4(value.get<bool>() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            return true;
        }
        if (type == "color"){
            TitleColor c;
            if (!parseTitleColor(value, c)) return false;
            out.value = Color::sRGBToLinear(Vector4(c.r, c.g, c.b, 1.0f));
            return true;
        }
        if (type == "font" || type == "image"){
            out.text = decodeAssetRef(value).path;
            return true;
        }
        if (!value.is_string()) return false;
        out.text = value.get<std::string>();
        return true;
    }

    std::shared_ptr<const TitleArtDoc> loadDocument(const std::string& path, std::string* error){
        std::string text;
        if (!readText(path, text)){
            if (error) *error = "cannot read " + path + " (a Realtime or Glyph Atlas title ships its .titleart)";
            return nullptr;
        }
        TitleArtParseResult parsed = parseTitleArt(text);
        if (!parsed.ok){
            if (error) *error = path + " does not parse: " + parsed.error.toString() + ". Open it in the Title Designer and save it again.";
            return nullptr;
        }
        return std::make_shared<const TitleArtDoc>(std::move(parsed.doc));
    }

}

bool TitleArtRuntime::isBuilt(){
    return true;
}

namespace {

    // a build's copy of the project's settings, once, before anything reads them; the editor has
    // no such file (syncTitleArtRuntime configures the scheduler from the project)
    void applyExportedSettings(){
        static bool applied = false;
        if (applied) return;
        applied = true;
        std::string text;
        if (!readText(TITLE_ART_RUNTIME_SETTINGS_PATH, text)) return;
        TitleArtRuntimeSettings settings = TitleArtScheduler::get().settings();
        if (parseTitleArtRuntimeSettings(text, settings)) TitleArtScheduler::get().configure(settings);
        else Log::error("Title Art: %s is not a version 1 settings file: the default budgets apply", TITLE_ART_RUNTIME_SETTINGS_PATH);
    }

}

std::shared_ptr<TitleArtLive> TitleArtRuntime::create(const std::string& titlePath, TitleArtRenderMode mode, std::string* error){
    applyExportedSettings();
    std::shared_ptr<const TitleArtDoc> doc = loadDocument(titlePath, error);
    if (!doc) return nullptr;
    const uint64_t hash = hashTitleArt(*doc).document;
    auto live = std::make_shared<TitleArtLive>(titlePath, mode, doc, hash);
    if (mode == TitleArtRenderMode::GLYPH_ATLAS) live->setGlyphPlan(planTitleGlyphAtlas(*doc));
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    if (live->has3D()){
        // S86 R26, R38: what the export cooked for this title (a build's; none in the editor)
        std::shared_ptr<TitleCooked3DSource> cooked = titleCooked3DFor(titlePath);
#if !(defined(JASPERENGINE_TITLE_ART_MESHER) && JASPERENGINE_TITLE_ART_MESHER)
        if (cooked) Log::print("Title Art '%s': its 3D layers draw the meshes the export cooked (this build has no 3D mesher)", titlePath.c_str());
        else Log::warn("Title Art '%s': this build has no 3D mesher and its export cooked no mesh for it: its 3D layers draw nothing. Export "
                       "the game again (Realtime 3D titles whose geometry cannot change ship their meshes)", titlePath.c_str());
#endif
        live->setCooked3D(std::move(cooked));
    }
#endif
    TitleArtScheduler::get().add(live.get());
    return live;
}

namespace {

    // S83's tier on this device: S56's platform group and tier (the editor's Game view simulation
    // sets both). S86's tier is tierFor's, set by the caller (TitleArtSystem) with the component's
    // Quality Tier
    TitleArtLiveState withTiers(const TitleArtLiveState& state){
        TitleArtLiveState out = state;
        out.fxTier = (int)titleFxTierFor(QualityResolver::getPlatformGroup(), QualityResolver::getTier());
        return out;
    }

    // R9 without the pixel path
    uint64_t contentHashOf(TitleArtLive& live, const TitleArtLiveState& state){
        uint64_t hash = hashCombine(live.documentHash(), (uint64_t)live.mode());
        for (const FieldOverride& o : fieldOverrides(*live.document(), state.overrides))
            hash = hashCombine(hash, fnv1a64(o.layerId + "\x1f" + o.path + "\x1f" + o.value.dump()));
        hash = hashCombine(hash, (uint64_t)std::llround(state.resolution * 8.0f));
        hash = hashCombine(hash, ((uint64_t)(uint32_t)state.region.x << 32) ^ (uint32_t)state.region.y);
        hash = hashCombine(hash, ((uint64_t)(uint32_t)state.region.width << 32) ^ (uint32_t)state.region.height);
        // S86 R37: a 3D title's tier (0 for every other title, so their keys stay as they were)
        if (state.tier != 0) hash = hashCombine(hash, 0x3d00u + (uint64_t)state.tier);
        hash = hashCombine(hash, live.animationOverridesHash());
        return hashCombine(hash, state.glowOutput ? 3u : 4u);
    }

}

uint64_t TitleArtRuntime::prebakeKeyOf(TitleArtLive& live, const TitleArtLiveState& state){
    const uint64_t hash = contentHashOf(live, state);
    return hash == 0 ? 1 : hash;
}

uint64_t TitleArtRuntime::stateHashOf(TitleArtLive& live, const TitleArtLiveState& given){
    // R9: the document's content, the effective values, the resolution, the pixel path, S86's
    // tier and S83's device tier (the prebake key leaves the pixel path and the device tier out: its
    // image serves either)
    const TitleArtLiveState state = withTiers(given);
    uint64_t hash = hashCombine(live.documentHash(), (uint64_t)live.mode());
    for (const FieldOverride& o : fieldOverrides(*live.document(), state.overrides))
        hash = hashCombine(hash, fnv1a64(o.layerId + "\x1f" + o.path + "\x1f" + o.value.dump()));
    hash = hashCombine(hash, (uint64_t)std::llround(state.resolution * 8.0f));
    hash = hashCombine(hash, ((uint64_t)(uint32_t)state.region.x << 32) ^ (uint32_t)state.region.y);
    hash = hashCombine(hash, ((uint64_t)(uint32_t)state.region.width << 32) ^ (uint32_t)state.region.height);
    hash = hashCombine(hash, titleArtForceRgba8() || (state.testFlags & TITLE_ART_TEST_NO_FLOAT_TARGETS) ? 1u : 2u);
    if (state.tier != 0) hash = hashCombine(hash, 0x3d00u + (uint64_t)state.tier);
    hash = hashCombine(hash, state.glowOutput ? 3u : 4u);
    hash = hashCombine(hash, (uint64_t)(uint32_t)state.fxTier << 8);
    hash = hashCombine(hash, live.animationOverridesHash());
    return hash == 0 ? 1 : hash;
}

namespace {

    // the request's overrides: the exposed values', then the animation's (S85 R18)
    std::vector<FieldOverride> withAnimation(const TitleArtLive& live, std::vector<FieldOverride> overrides){
        const std::vector<FieldOverride>& animation = live.animationOverrides();
        overrides.insert(overrides.end(), animation.begin(), animation.end());
        return overrides;
    }

    std::vector<uint32_t> decodeUtf8(const std::string& text){
        std::vector<uint32_t> out;
        for (size_t i = 0; i < text.size();){
            const unsigned char c = (unsigned char)text[i];
            uint32_t cp = c;
            size_t n = 1;
            if (c >= 0xF0){ cp = c & 0x07; n = 4; }
            else if (c >= 0xE0){ cp = c & 0x0F; n = 3; }
            else if (c >= 0xC0){ cp = c & 0x1F; n = 2; }
            if (i + n > text.size()) break;
            for (size_t k = 1; k < n; k++) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
            out.push_back(cp);
            i += n;
        }
        return out;
    }

    void appendUtf8(std::string& out, uint32_t cp){
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800){ out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000){ out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else{ out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

}

void TitleArtRuntime::setState(TitleArtLive& live, const TitleArtLiveState& given){
    // S87 R17: a new language resolves the document again; the hash below follows it
    live.setLanguage(given.language);
    const TitleArtLiveState state = withTiers(given);
    live.setLiveBudgetMs(TitleArtScheduler::get().liveBudgetMs());
    // R55: with a Characters set (the editor's Play simulating the build), characters outside it
    // draw as nothing, as a subset font would leave them; the title says which once per set
    if (!state.characters.empty()){
        std::vector<uint32_t> allowed = decodeUtf8(state.characters);
        std::sort(allowed.begin(), allowed.end());
        TitleArtLiveState filtered = state;
        std::vector<uint32_t> missing;
        for (TitleArtOverride& o : filtered.overrides){
            if (o.type != "text") continue;
            std::string kept;
            for (uint32_t cp : decodeUtf8(o.text)){
                const bool always = cp == ' ' || cp == '\n' || cp == '\t';
                if (always || std::binary_search(allowed.begin(), allowed.end(), cp)) appendUtf8(kept, cp);
                else if (std::find(missing.begin(), missing.end(), cp) == missing.end()) missing.push_back(cp);
            }
            o.text = kept;
        }
        std::string missingText;
        for (uint32_t cp : missing) appendUtf8(missingText, cp);
        live.setMissingCharacters(missingText);
        const uint64_t hash = stateHashOf(live, filtered);
        live.setState(filtered, hash, withAnimation(live, fieldOverrides(*live.document(), filtered.overrides)));
        return;
    }
    live.setMissingCharacters(std::string());
    const uint64_t hash = stateHashOf(live, state);
    live.setState(state, hash, withAnimation(live, fieldOverrides(*live.document(), state.overrides)));
}

std::shared_ptr<titleanim::TitleAnimLibrary> TitleArtRuntime::compileAnimations(const std::string& titlePath, std::string* error){
    std::shared_ptr<const TitleArtDoc> doc = loadDocument(titlePath, error);
    if (!doc) return nullptr;
    if (!hasAnimations(*doc)) return std::make_shared<titleanim::TitleAnimLibrary>();
    TitleTextLayout layout(TitleArtScheduler::get().assetResolver());
    return titleart::compileAnimations(*doc, &layout, nullptr);
}

bool TitleArtRuntime::prepareAnimation(TitleArtLive& live, const titleanim::TitleAnimPlayer& player, const std::string& name){
    const titleanim::CompiledTitleAnimation* animation = player.find(name);
    if (!animation || animation->rerender != titleanim::RerenderPolicy::Prerender || live.mode() != TitleArtRenderMode::REALTIME) return true;
    if (live.prerendering()) return true;
    const TitleAnimPlan builtPlan = buildAnimPlan(*live.document(), player.library()->animations, live.mode());
    const AnimationPlan* plan = builtPlan.find(animation->name);
    if (!plan) return true;
    std::map<const titleanim::CompiledTitleTrack*, bool> rerendered;
    for (const TrackPlan& t : plan->tracks)
        if ((t.how == "prerender" || t.how == "rerender") && t.track < animation->tracks.size()) rerendered[&animation->tracks[t.track]] = true;
    if (rerendered.empty()) return true;
    // every sample a playback asks for (R18's k / rerender_fps), the hash it asks with
    const double fps = std::max(1.0f, animation->rerenderFps);
    const int count = std::max(1, (int)std::ceil(animation->duration * fps - 1e-4) + 1);
    const TitleDisplayImage& front = live.front();
    const size_t frameBytes = front.valid() ? front.bytes : (size_t)live.document()->canvasWidth() * live.document()->canvasHeight() * 4u;
    if ((size_t)count * frameBytes > TitleArtScheduler::get().cacheBudgetBytes()){
        Log::warn("Title Art: prerender of '%s' needs %.1f MiB of frames, over the Cache Budget: it plays live", name.c_str(),
                  (double)count * frameBytes / (1024.0 * 1024.0));
        return false;
    }
    titleanim::TitleAnimPlayer sampler;
    sampler.setLibrary(player.library());
    std::vector<TitleArtLive::PrerenderFrame> frames;
    for (int k = 0; k < count; k++){
        sampler.sample(animation->name, k / fps);
        PlayerPose pose;
        playerPose(*live.document(), sampler, true, [&](const titleanim::CompiledTitleTrack& t){ return rerendered.count(&t) > 0; }, pose);
        frames.push_back({pose.values.overrides(), hashCombine(pose.values.hash(), 0)});
    }
    live.setPrerenderFrames(std::move(frames));
    return true;
}

bool TitleArtRuntime::isPrerendering(const TitleArtLive& live){
    return live.prerendering();
}

void TitleArtRuntime::setAnimation(TitleArtLive& live, const titleanim::TitleAnimPlayer& player, int animatingTitles){
    const std::shared_ptr<const titleanim::TitleAnimLibrary>& library = player.library();
    // R18: one frame of a prepare at a time, when nothing else renders; each lands in the frame
    // cache as the next one replaces it
    if (live.prerendering() && !player.active()){
        if (!live.hasWork()){
            const TitleArtLive::PrerenderFrame& frame = live.nextPrerenderFrame();
            live.setAnimationOverrides(frame.overrides, frame.hash);
            live.advancePrerender();
        }
        return;
    }
    if (!library || !player.active()){
        if (!live.animationOverrides().empty() || live.animating()){
            live.setAnimationOverrides({}, 0);
            live.setGlyphPoses(nullptr);
            live.setComposePose(nullptr);
        }
        live.setAnimating(false, 1.0);
        return;
    }
    // the plan of this library in this mode, made once (the library changes with the document)
    if (live.animationPlanKey != library.get()){
        live.animationPlanKey = library.get();
        live.animationPlanCache = std::make_shared<TitleAnimPlan>(buildAnimPlan(*live.document(), library->animations, live.mode()));
    }
    const TitleAnimPlan& plan = *static_cast<const TitleAnimPlan*>(live.animationPlanCache.get());
    // R18: what re-renders (rerender, prerender, plate re-render) is a request per sample time;
    // title-live tracks are the companion's, glyph cells compose at the display rate
    std::map<const titleanim::CompiledTitleTrack*, bool> rerendered;
    for (const titleanim::CompiledTitleAnimation& a : library->animations){
        const AnimationPlan* ap = plan.find(a.name);
        if (!ap) continue;
        for (const TrackPlan& t : ap->tracks){
            const bool rerender = t.how == "rerender" || t.how == "prerender" || t.how == "plate_rerender";
            if (rerender && t.track < a.tracks.size()) rerendered[&a.tracks[t.track]] = true;
        }
    }
    PlayerPose pose;
    playerPose(*live.document(), player, true, [&](const titleanim::CompiledTitleTrack& t){ return rerendered.count(&t) > 0; }, pose);
    uint64_t composeHash = 0;
    if (live.mode() == TitleArtRenderMode::REALTIME){
        // R14, R15: layer splits and cells compose at the display rate, at unquantized time
        std::map<const titleanim::CompiledTitleTrack*, bool> split;
        for (const titleanim::CompiledTitleAnimation& a : library->animations){
            const AnimationPlan* ap = plan.find(a.name);
            if (!ap) continue;
            for (const TrackPlan& t : ap->tracks) if (t.how == "layer_split" && t.track < a.tracks.size()) split[&a.tracks[t.track]] = true;
        }
        auto compose = std::make_shared<TitleComposePose>();
        if (!split.empty()){
            PlayerPose layerPose;
            playerPose(*live.document(), player, false, [&](const titleanim::CompiledTitleTrack& t){ return split.count(&t) > 0; }, layerPose);
            for (const FieldOverride& o : layerPose.values.overrides()){
                compose->layers[o.layerId][o.path] = o.value;
                composeHash = hashCombine(composeHash, fnv1a64(o.layerId + "|" + o.path + "|" + o.value.dump()));
            }
        }
        for (auto& [layer, glyphs] : pose.glyphs){
            for (const auto& entry : glyphs) composeHash = hashCombine(composeHash, hashCombine(fnv1a64(layer), hashCombine(entry.first, glyphPoseHash(entry.second))));
            compose->glyphs[layer] = glyphs;
        }
        live.setComposePose(compose->empty() || !player.isPlaying() ? nullptr : compose);
    }else{
        // a Glyph Atlas title's own layer: its cells draw through the atlas compose
        std::shared_ptr<const std::map<uint32_t, GlyphPose>> glyphs;
        if (auto found = pose.glyphs.find(live.glyphAtlasPlan().textLayer); found != pose.glyphs.end() && live.glyphAtlasPlan().fallback.empty()){
            for (const auto& entry : found->second) composeHash = hashCombine(composeHash, hashCombine(entry.first, glyphPoseHash(entry.second)));
            glyphs = std::make_shared<const std::map<uint32_t, GlyphPose>>(std::move(found->second));
        }
        live.setGlyphPoses(glyphs);
    }
    live.setAnimationOverrides(pose.values.overrides(), hashCombine(pose.values.hash(), composeHash));
    const int n = std::max(1, animatingTitles);
    live.setAnimating(player.isPlaying(), 1.0 / (double)n);
}

TitleArtLiveStatus TitleArtRuntime::status(const TitleArtLive& live){
    TitleArtLiveStatus s = live.status();
    s.poolBytes = TitleArtScheduler::get().poolBytes();
    return s;
}

TextureRender* TitleArtRuntime::display(TitleArtLive& live){
    return live.front().texture();
}

Framebuffer* TitleArtRuntime::displayFramebuffer(TitleArtLive& live){
    return live.front().framebuffer();
}

Framebuffer* TitleArtRuntime::previousFramebuffer(TitleArtLive& live, float* blend){
    const TitleDisplayImage* previous = live.previous(blend);
    return previous ? previous->framebuffer() : nullptr;
}

Framebuffer* TitleArtRuntime::glowFramebuffer(TitleArtLive& live){
    return live.front().glowFramebuffer();
}

void TitleArtRuntime::invalidate(TitleArtLive& live){
    live.invalidate();
}

float TitleArtRuntime::referenceScale(){
    const int height = TitleArtScheduler::get().settings().referenceHeight;
    const unsigned int canvas = Engine::getCanvasHeight();
    return height > 0 && canvas > 0 ? (float)height / (float)canvas : 1.0f;
}

int TitleArtRuntime::tierFor(const TitleArtLive& live, const std::string& qualityTier){
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
    if (!live.has3D()) return 0;
    const int platform = std::clamp((int)Engine::getPlatform(), 0, 5);
    // the project's Realtime 3D Quality: the build's export wrote it; the editor's settings else
    TitleTier project = (TitleTier)std::clamp(TitleArtScheduler::get().settings().realtime3DTier[platform], 1, 3);
    titleCookedProjectTier(platform, project);
    // the editor's Game view simulating another platform group (S56) takes that group's default
    // (desktop High, mobile and web Low, as Android and HTML5)
    const DevicePlatformGroup group = QualityResolver::getPlatformGroup();
    if (group != QualityResolver::platformGroupOf(platform)) project = group == DevicePlatformGroup::Desktop ? TitleTier::High : TitleTier::Low;
    const std::optional<DeviceTier> device = QualityResolver::getTier();
    const bool gles = Engine::getGraphicBackend() == GraphicBackend::GLES3;
    return (int)resolveTitleTier(qualityTier, project, device.has_value(), device.value_or(DeviceTier::High), gles);
#else
    (void)live;
    (void)qualityTier;
    return 0;
#endif
}

namespace {

    // a layer by path (names joined by '/', "#N" for the Nth same-named sibling after the first,
    // the EntityPath grammar the ops use) or by id
    const Json* layerAtPath(const Json& root, const std::string& path){
        if (const Json* byId = findLayer(root, path)) return byId;
        static const Json none = Json::array();
        auto top = root.find("layers");
        const Json* siblings = top != root.end() ? &*top : &none;
        const Json* found = nullptr;
        size_t start = 0;
        while (start <= path.size()){
            const size_t slash = path.find('/', start);
            std::string name = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            int nth = 0;
            const size_t hash = name.rfind('#');
            if (hash != std::string::npos && hash + 1 < name.size() &&
                name.find_first_not_of("0123456789", hash + 1) == std::string::npos){
                nth = std::atoi(name.c_str() + hash + 1);
                name = name.substr(0, hash);
            }
            found = nullptr;
            if (!siblings->is_array()) return nullptr;
            int seen = 0;
            for (const Json& layer : *siblings){
                if (!layer.is_object() || layer.value("name", std::string()) != name) continue;
                if (seen++ == nth){
                    found = &layer;
                    break;
                }
            }
            if (!found) return nullptr;
            if (slash == std::string::npos) break;
            siblings = &layerChildren(*found);
            start = slash + 1;
        }
        return found;
    }

}

bool TitleArtRuntime::meshStats(const TitleArtLive& live, const std::string& layerPath, TitleArtMeshStats& out, bool& found, std::string* error){
    found = false;
    const Json* layer = live.document() ? layerAtPath(live.document()->root, layerPath) : nullptr;
    if (!layer || layer->value("dimension", std::string()) != "3d"){
        if (error && live.document()){
            std::string names;
            forEachLayer(live.document()->root, [&names](const Json& l, const Json*, int){
                if (l.value("dimension", std::string()) == "3d") names += (names.empty() ? "" : ", ") + l.value("name", std::string());
                return true;
            });
            *error = (layer ? "the layer '" + layerPath + "' is not Dimension 3D" : "no layer '" + layerPath + "'") +
                     (names.empty() ? std::string(": the title has no 3D layer") : ": its 3D layers are " + names);
        }
        return false;
    }
    const std::string id = layer->value("id", std::string());
    for (const TitleArtMeshStats& m : live.front().meshes){
        if (m.layerId != id) continue;
        out = m;
        found = true;
        break;
    }
    return true;
}

void TitleArtRuntime::setPrebaked(TitleArtLive& live, const std::string& texturePath, uint64_t stateHash){
    // R11: the export rendered this state in the editor; its file is a texture like any PNG
    TextureData probe;
    if (!probe.loadTextureFromFile(texturePath.c_str())) return;
    TitleDisplayImage image;
    image.prebaked = std::make_shared<Texture>(texturePath);
    image.width = probe.getWidth();
    image.height = probe.getHeight();
    image.bytes = (size_t)image.width * image.height * 4;
    image.hash = stateHash;
    const TitleArtRectI& region = live.state().region;
    image.frame = {region.x, region.y, region.width, region.height};
    live.setPrebaked(std::move(image));
}

bool TitleArtRuntime::readLanguages(const std::string& titlePath, std::vector<std::string>& out, std::string* error){
    out.clear();
    std::shared_ptr<const TitleArtDoc> doc = loadDocument(titlePath, error);
    if (!doc) return false;
    out = titleLanguageCodes(doc->root);
    if (titleSourceLanguage(doc->root).empty()) out.insert(out.begin(), std::string());
    return true;
}

bool TitleArtRuntime::readExposed(const std::string& titlePath, std::vector<TitleArtExposedParam>& out, std::string* error){
    out.clear();
    std::shared_ptr<const TitleArtDoc> doc = loadDocument(titlePath, error);
    if (!doc) return false;
    const Json& root = doc->root;
    for (const Json& item : root.value("exposed", Json::array())){
        if (!item.is_object()) continue;
        TitleArtExposedParam p;
        p.name = item.value("name", std::string());
        p.label = item.value("label", std::string());
        p.type = item.value("type", std::string("text"));
        if (item.contains("min") && item["min"].is_number()){ p.hasMin = true; p.min = item["min"].get<double>(); }
        if (item.contains("max") && item["max"].is_number()){ p.hasMax = true; p.max = item["max"].get<double>(); }
        for (const Json& o : item.value("options", Json::array())) if (o.is_string()) p.options.push_back(o.get<std::string>());
        p.defaultValue.name = p.name;
        p.defaultValue.type = p.type;
        const Json targets = item.value("targets", Json::array());
        p.targetsText = !targets.empty();
        for (const Json& target : targets){
            const std::string layerId = target.value("layer", std::string());
            const std::string field = target.value("field", std::string());
            const Json* layer = layerId.empty() ? nullptr : findLayer(root, layerId);
            if (!layer || field != "text" || layerTypeOf(*layer) != LayerType::Text) p.targetsText = false;
            if (p.hasDefault) continue;
            const Json* value = layer ? getTitleArtField(*layer, "layer", field) : getTitleArtField(root, "document", field);
            if (value && fieldValue(*value, p.type, p.defaultValue)) p.hasDefault = true;
        }
        out.push_back(std::move(p));
    }
    return true;
}

void TitleArtRuntime::runFrame(double dt, bool editorEditMode){
    TitleArtScheduler::get().runFrame(dt, editorEditMode);
}

void TitleArtRuntime::releaseAll(){
    TitleArtScheduler::get().releaseAll();
}

void TitleArtRuntime::setFrameBudget(double ms){
    TitleArtScheduler::get().setFrameBudgetOverride(ms);
}

double TitleArtRuntime::getFrameBudget(bool editorEditMode){
    return TitleArtScheduler::get().frameBudgetMs(editorEditMode);
}
