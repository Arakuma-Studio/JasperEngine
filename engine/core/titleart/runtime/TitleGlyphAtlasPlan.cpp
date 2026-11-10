// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleGlyphAtlasPlan.h"

#include "titleart/TitleEffectRegistry.h"

#include <functional>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    std::string str(const Json& object, const char* key){
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // the chain of groups above a layer, nearest first
    bool ancestorsOf(const Json& layers, const std::string& id, std::vector<const Json*>& chain){
        for (const Json& layer : layers){
            if (!layer.is_object()) continue;
            if (str(layer, "id") == id) return true;
            auto children = layer.find("children");
            if (children != layer.end() && children->is_array() && ancestorsOf(*children, id, chain)){
                chain.push_back(&layer);
                return true;
            }
        }
        return false;
    }

    Json setOp(const std::string& layer, const std::string& path, const Json& value){
        return Json{{"ops", Json::array({Json{{"op", "set"}, {"layer", "id:" + layer}, {"values", Json{{path, value}}}}})}};
    }

}

std::vector<std::string> TitleGlyphAtlasPlan::unsafeLabels() const{
    std::vector<std::string> out;
    for (const TitleGlyphAtlasItem& item : unsafe) out.push_back(item.label);
    return out;
}

std::vector<std::string> TitleGlyphAtlasPlan::approximateLabels() const{
    std::vector<std::string> out;
    for (const TitleGlyphAtlasItem& item : approximate) out.push_back(item.label);
    return out;
}

TitleGlyphAtlasPlan jasperengine::titleart::planTitleGlyphAtlas(const TitleArtDoc& doc){
    TitleGlyphAtlasPlan plan;
    const Json& root = doc.root;
    // R25: the glyph layer, else the first text layer the exposed `text` drives
    std::string id;
    if (auto runtime = root.find("runtime"); runtime != root.end() && runtime->is_object()) id = str(*runtime, "glyph_layer");
    if (id.empty()){
        for (const Json& exposed : root.value("exposed", Json::array())){
            if (str(exposed, "name") != "text") continue;
            for (const Json& target : exposed.value("targets", Json::array())){
                const Json* layer = findLayer(root, str(target, "layer"));
                if (layer && layerTypeOf(*layer) == LayerType::Text && str(target, "field") == "text"){
                    id = str(target, "layer");
                    break;
                }
            }
            break;
        }
    }
    const Json* layer = id.empty() ? nullptr : findLayer(root, id);
    if (!layer || layerTypeOf(*layer) != LayerType::Text){
        plan.fallback = "no text layer is exposed as 'text' (Exposed Parameters > Expose Text in the Title Designer)";
        return plan;
    }
    plan.textLayer = id;
    // R30: no 3D text
    if (str(*layer, "dimension") == "3d"){
        plan.fallback = "3D text";
        return plan;
    }
    // R25: ancestor groups may have normal blend, opacity and transforms only
    std::vector<const Json*> chain;
    // by reference: the chain points into the document
    static const Json noLayers = Json::array();
    auto layers = root.find("layers");
    ancestorsOf(layers != root.end() && layers->is_array() ? *layers : noLayers, id, chain);
    for (const Json* group : chain){
        const std::string blend = str(*group, "blend");
        if (!blend.empty() && blend != "normal"){
            plan.fallback = "group '" + str(*group, "name") + "' blends " + blend;
            return plan;
        }
        if (group->contains("masks") && (*group)["masks"].is_array() && !(*group)["masks"].empty()){
            plan.fallback = "group '" + str(*group, "name") + "' has a mask";
            return plan;
        }
        if (group->contains("effects") && (*group)["effects"].is_array() && !(*group)["effects"].empty()){
            plan.fallback = "group '" + str(*group, "name") + "' has effects";
            return plan;
        }
    }

    // R31: what does not apply per glyph
    if (auto warp = layer->find("warp"); warp != layer->end() && warp->is_object() && str(*warp, "style") != "none" && !str(*warp, "style").empty())
        plan.unsafe.push_back({"Warp", "warp.style", setOp(id, "warp.style", "none")});
    if (auto distort = layer->find("distort"); distort != layer->end() && distort->is_object() && str(*distort, "mode") != "none" && !str(*distort, "mode").empty())
        plan.unsafe.push_back({"Distort", "distort.mode", setOp(id, "distort.mode", "none")});
    if (auto style = layer->find("style"); style != layer->end() && style->is_object()){
        const Json rims = style->value("rims", Json::array());
        for (size_t i = 0; i < rims.size(); i++){
            const Json& rim = rims[i];
            if (!rim.value("enabled", true)) continue;
            const std::string kind = rim.contains("plate") && rim["plate"].is_object() ? str(rim["plate"], "kind") : std::string();
            if (kind == "hull" || kind == "box"){
                const std::string path = "style.rims[" + std::to_string(i) + "].plate.kind";
                // hull and box plates draw as contour per glyph
                plan.unsafe.push_back({std::string("Plate (") + (kind == "hull" ? "Hull" : "Box") + ")", path, setOp(id, path, "contour")});
            }
        }
    }
    if (layer->contains("masks") && (*layer)["masks"].is_array() && !(*layer)["masks"].empty())
        plan.unsafe.push_back({"Layer Masks", "masks", nullptr});
    const Json effects = layer->value("effects", Json::array());
    for (size_t i = 0; i < effects.size(); i++){
        const Json& entry = effects[i];
        if (!entry.value("enabled", true)) continue;
        const std::string type = str(entry, "type");
        const EffectDef* def = findEffect(type);
        if (!def) continue;
        const std::string label = def->label && *def->label ? def->label : type;
        if (def->glyphSafety == GlyphSafety::Unsafe)
            plan.unsafe.push_back({label, "fx:" + type, setOp(id, "effects[" + std::to_string(i) + "].enabled", false)});
        else if (def->glyphSafety == GlyphSafety::Approximate)
            plan.approximate.push_back({label, "fx:" + type, nullptr});
    }
    // generators that follow the text layer read its whole outline
    forEachLayer(root, [&](const Json& other, const Json*, int){
        if (layerTypeOf(other) != LayerType::Generator) return true;
        const Json generator = other.value("generator", Json::object());
        if (str(generator, "follow") == id || str(other, "follow") == id){
            const GeneratorDef* def = findGenerator(str(generator, "type"));
            const std::string label = def && def->label && *def->label ? def->label : str(other, "name");
            plan.unsafe.push_back({label + " (follows the text)", "layer:" + str(other, "id"), nullptr});
        }
        return true;
    });
    return plan;
}
