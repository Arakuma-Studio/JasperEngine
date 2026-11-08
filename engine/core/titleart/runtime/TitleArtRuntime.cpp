// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// TitleArtRuntime with the `titleart` module (docs/specs/S81-title-art-runtime.md R9-R24): the
// core's seam onto TitleArtLive and the engine's one TitleArtScheduler. A title's state turns
// into S80 field overrides through the document's Exposed parameters (R5), and into the state
// hash the scheduler, the cache and the export's prebake share (R9, R11).

#include "subsystem/TitleArtRuntime.h"

#include "io/Data.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleTranslations.h"
#include "titleart/TitleArtIO.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "util/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

std::shared_ptr<TitleArtLive> TitleArtRuntime::create(const std::string& titlePath, TitleArtRenderMode mode, std::string* error){
    std::shared_ptr<const TitleArtDoc> doc = loadDocument(titlePath, error);
    if (!doc) return nullptr;
    const uint64_t hash = hashTitleArt(*doc).document;
    auto live = std::make_shared<TitleArtLive>(titlePath, mode, doc, hash);
    if (mode == TitleArtRenderMode::GLYPH_ATLAS) live->setGlyphPlan(planTitleGlyphAtlas(*doc));
    TitleArtScheduler::get().add(live.get());
    return live;
}

namespace {

    // R9 without the pixel path
    uint64_t contentHashOf(TitleArtLive& live, const TitleArtLiveState& state){
        uint64_t hash = hashCombine(live.documentHash(), (uint64_t)live.mode());
        for (const FieldOverride& o : fieldOverrides(*live.document(), state.overrides))
            hash = hashCombine(hash, fnv1a64(o.layerId + "\x1f" + o.path + "\x1f" + o.value.dump()));
        hash = hashCombine(hash, (uint64_t)std::llround(state.resolution * 8.0f));
        hash = hashCombine(hash, ((uint64_t)(uint32_t)state.region.x << 32) ^ (uint32_t)state.region.y);
        hash = hashCombine(hash, ((uint64_t)(uint32_t)state.region.width << 32) ^ (uint32_t)state.region.height);
        return hashCombine(hash, state.glowOutput ? 3u : 4u);
    }

}

uint64_t TitleArtRuntime::prebakeKeyOf(TitleArtLive& live, const TitleArtLiveState& state){
    const uint64_t hash = contentHashOf(live, state);
    return hash == 0 ? 1 : hash;
}

uint64_t TitleArtRuntime::stateHashOf(TitleArtLive& live, const TitleArtLiveState& state){
    // R9: the document's content, the effective values, the resolution and the pixel path
    uint64_t hash = hashCombine(live.documentHash(), (uint64_t)live.mode());
    for (const FieldOverride& o : fieldOverrides(*live.document(), state.overrides))
        hash = hashCombine(hash, fnv1a64(o.layerId + "\x1f" + o.path + "\x1f" + o.value.dump()));
    hash = hashCombine(hash, (uint64_t)std::llround(state.resolution * 8.0f));
    hash = hashCombine(hash, ((uint64_t)(uint32_t)state.region.x << 32) ^ (uint32_t)state.region.y);
    hash = hashCombine(hash, ((uint64_t)(uint32_t)state.region.width << 32) ^ (uint32_t)state.region.height);
    hash = hashCombine(hash, titleArtForceRgba8() || (state.testFlags & TITLE_ART_TEST_NO_FLOAT_TARGETS) ? 1u : 2u);
    hash = hashCombine(hash, state.glowOutput ? 3u : 4u);
    return hash == 0 ? 1 : hash;
}

namespace {

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

void TitleArtRuntime::setState(TitleArtLive& live, const TitleArtLiveState& state){
    // S87 R17: a new language resolves the document again; the hash below follows it
    live.setLanguage(state.language);
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
        live.setState(filtered, hash, fieldOverrides(*live.document(), filtered.overrides));
        return;
    }
    live.setMissingCharacters(std::string());
    const uint64_t hash = stateHashOf(live, state);
    live.setState(state, hash, fieldOverrides(*live.document(), state.overrides));
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
