// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// TitleArtRuntime without the `titleart` module (JASPERENGINE_TITLE_ART OFF, docs/specs/
// S81-title-art-runtime.md R51): no title renders at runtime; a Realtime or Glyph Atlas title
// shows its bake and TitleArtSystem says why once. The Exposed parameters are still read (a
// light JSON walk of the document, which the build ships for such titles), so scripts keep
// their names, types and errors.

#include "subsystem/TitleArtRuntime.h"
#include "titleanim/TitleAnimPlayer.h"

#include "io/Data.h"
#include "json.hpp"
#include "util/Color.h"

#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    using Json = nlohmann::json;

    const Json* findLayer(const Json& layers, const std::string& id){
        if (!layers.is_array()) return nullptr;
        for (const Json& layer : layers){
            if (!layer.is_object()) continue;
            auto it = layer.find("id");
            if (it != layer.end() && it->is_string() && it->get<std::string>() == id) return &layer;
            auto children = layer.find("children");
            if (children != layer.end())
                if (const Json* found = findLayer(*children, id)) return found;
        }
        return nullptr;
    }

    // a plain dotted path ("text", "style.face.color"); the shorthands need the module
    const Json* walk(const Json& object, const std::string& path){
        const Json* current = &object;
        size_t start = 0;
        while (start <= path.size()){
            const size_t dot = path.find('.', start);
            const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (!current->is_object()) return nullptr;
            auto it = current->find(key);
            if (it == current->end()) return nullptr;
            current = &*it;
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return current;
    }

    bool hexColor(const std::string& text, Vector4& linear){
        if (text.size() != 7 || text[0] != '#') return false;
        char* end = nullptr;
        const unsigned long value = std::strtoul(text.c_str() + 1, &end, 16);
        if (!end || *end) return false;
        linear = Color::sRGBToLinear(Vector4(((value >> 16) & 0xff) / 255.0f, ((value >> 8) & 0xff) / 255.0f, (value & 0xff) / 255.0f, 1.0f));
        return true;
    }

}

bool TitleArtRuntime::isBuilt(){
    return false;
}

std::shared_ptr<TitleArtLive> TitleArtRuntime::create(const std::string&, TitleArtRenderMode, std::string* error){
    if (error) *error = "this build has no Title Art module (titleart)";
    return nullptr;
}

void TitleArtRuntime::setState(TitleArtLive&, const TitleArtLiveState&){
}

TitleArtLiveStatus TitleArtRuntime::status(const TitleArtLive&){
    return TitleArtLiveStatus();
}

TextureRender* TitleArtRuntime::display(TitleArtLive&){
    return nullptr;
}

Framebuffer* TitleArtRuntime::displayFramebuffer(TitleArtLive&){
    return nullptr;
}

Framebuffer* TitleArtRuntime::previousFramebuffer(TitleArtLive&, float* blend){
    if (blend) *blend = 1.0f;
    return nullptr;
}

Framebuffer* TitleArtRuntime::glowFramebuffer(TitleArtLive&){
    return nullptr;
}

void TitleArtRuntime::invalidate(TitleArtLive&){
}

float TitleArtRuntime::referenceScale(){
    return 1.0f;
}

int TitleArtRuntime::tierFor(const TitleArtLive&, const std::string&){
    return 0;
}

bool TitleArtRuntime::meshStats(const TitleArtLive&, const std::string&, TitleArtMeshStats&, bool& found, std::string* error){
    found = false;
    if (error) *error = "this build has no Title Art module (titleart)";
    return false;
}

void TitleArtRuntime::setPrebaked(TitleArtLive&, const std::string&, uint64_t){
}

uint64_t TitleArtRuntime::stateHashOf(TitleArtLive&, const TitleArtLiveState&){
    return 0;
}

uint64_t TitleArtRuntime::prebakeKeyOf(TitleArtLive&, const TitleArtLiveState&){
    return 0;
}

void TitleArtRuntime::runFrame(double, bool){
}

void TitleArtRuntime::releaseAll(){
}

std::shared_ptr<titleanim::TitleAnimLibrary> TitleArtRuntime::compileAnimations(const std::string&, std::string* error){
    // without the module a title plays what its bake wrote: <file>.anim.json (S85 R60)
    if (error) *error = "this build has no Title Art module";
    return nullptr;
}

bool TitleArtRuntime::prepareAnimation(TitleArtLive&, const titleanim::TitleAnimPlayer&, const std::string&){
    return true;
}

bool TitleArtRuntime::isPrerendering(const TitleArtLive&){
    return false;
}

void TitleArtRuntime::setAnimation(TitleArtLive&, const titleanim::TitleAnimPlayer&, int){
}

void TitleArtRuntime::setFrameBudget(double){
}

double TitleArtRuntime::getFrameBudget(bool){
    return 2.0;
}

bool TitleArtRuntime::readLanguages(const std::string& titlePath, std::vector<std::string>& out, std::string* error){
    out.clear();
    Data data;
    if (data.open(titlePath.c_str()) != FILEDATA_OK){
        if (error) *error = "cannot read " + titlePath;
        return false;
    }
    const Json root = Json::parse(std::string(reinterpret_cast<const char*>(data.getMemPtr()), data.length()), nullptr, false);
    if (root.is_discarded() || !root.is_object()){
        if (error) *error = titlePath + " is not a Title Art document";
        return false;
    }
    auto source = root.find("source_language");
    out.push_back(source != root.end() && source->is_string() ? source->get<std::string>() : std::string());
    if (auto languages = root.find("languages"); languages != root.end() && languages->is_array()){
        for (const Json& l : *languages){
            if (l.is_object() && l.contains("code") && l["code"].is_string()) out.push_back(l["code"].get<std::string>());
        }
    }
    return true;
}

bool TitleArtRuntime::readExposed(const std::string& titlePath, std::vector<TitleArtExposedParam>& out, std::string* error){
    out.clear();
    Data data;
    if (data.open(titlePath.c_str()) != FILEDATA_OK){
        if (error) *error = "cannot read " + titlePath;
        return false;
    }
    const Json root = Json::parse(std::string(reinterpret_cast<const char*>(data.getMemPtr()), data.length()), nullptr, false);
    if (root.is_discarded() || !root.is_object()){
        if (error) *error = titlePath + " is not a Title Art document";
        return false;
    }
    auto exposed = root.find("exposed");
    if (exposed == root.end() || !exposed->is_array()) return true;
    const Json layers = root.value("layers", Json::array());
    for (const Json& item : *exposed){
        if (!item.is_object()) continue;
        TitleArtExposedParam p;
        p.name = item.value("name", std::string());
        p.label = item.value("label", std::string());
        p.type = item.value("type", std::string("text"));
        if (p.name.empty()) continue;
        if (item.contains("min") && item["min"].is_number()){ p.hasMin = true; p.min = item["min"].get<double>(); }
        if (item.contains("max") && item["max"].is_number()){ p.hasMax = true; p.max = item["max"].get<double>(); }
        if (item.contains("options") && item["options"].is_array())
            for (const Json& o : item["options"]) if (o.is_string()) p.options.push_back(o.get<std::string>());
        p.defaultValue.name = p.name;
        p.defaultValue.type = p.type;
        const Json targets = item.value("targets", Json::array());
        p.targetsText = !targets.empty();
        for (const Json& target : targets){
            const std::string layerId = target.is_object() ? target.value("layer", std::string()) : std::string();
            const std::string field = target.is_object() ? target.value("field", std::string()) : std::string();
            const Json* layer = findLayer(layers, layerId);
            if (!layer || field != "text" || layer->value("type", std::string()) != "text") p.targetsText = false;
            if (p.hasDefault || !layer) continue;
            const Json* value = walk(*layer, field);
            if (!value) continue;
            p.hasDefault = true;
            if (value->is_string()){
                Vector4 linear;
                if (p.type == "color" && hexColor(value->get<std::string>(), linear)) p.defaultValue.value = linear;
                else p.defaultValue.text = value->get<std::string>();
            }else if (value->is_number()){
                p.defaultValue.value = Vector4((float)value->get<double>(), 0.0f, 0.0f, 0.0f);
            }else if (value->is_boolean()){
                p.defaultValue.value = Vector4(value->get<bool>() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
            }else if (value->is_object() && value->contains("path")){
                p.defaultValue.text = value->value("path", std::string());
            }else{
                p.hasDefault = false;
            }
        }
        out.push_back(std::move(p));
    }
    return true;
}
