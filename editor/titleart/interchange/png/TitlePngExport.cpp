// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The parts plan (each layer alone on the shared frame; split titles as rim N alone with the
// face hidden and its width cumulative, the face, and fx) follows Pachislop src/io/export.ts
// (exportLayerParts) and src/io/psd.ts (titleParts) by SafEight, used with the author's
// permission.

#include "TitlePngExport.h"

#include "titleart/TitleArtIO.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleRectI;

namespace {

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::string slug(const std::string& name) {
        std::string out;
        bool gap = false;
        for (unsigned char c : name) {
            if (c < 128 && std::isalnum(c)) {
                if (gap && !out.empty()) out += '_';
                out += (char)std::tolower(c);
                gap = false;
            } else {
                gap = true;
            }
        }
        return out.empty() ? "layer" : out;
    }

    Json* findMutable(Json& list, const std::string& id) {
        if (!list.is_array()) return nullptr;
        for (Json& layer : list) {
            if (str(layer, "id") == id) return &layer;
            if (auto children = layer.find("children"); children != layer.end()) {
                if (Json* found = findMutable(*children, id)) return found;
            }
        }
        return nullptr;
    }

    // the visible leaves of the tree in draw order, with their paths
    void leaves(const Json& list, const std::string& prefix, bool parentHidden, std::vector<TitlePart>& out, std::set<std::string>& names) {
        if (!list.is_array()) return;
        std::map<std::string, int> counts;
        for (const Json& layer : list) {
            const std::string name = str(layer, "name");
            const int n = counts[name]++;
            const std::string path = prefix + name + (n ? "#" + std::to_string(n) : "");
            const bool hidden = parentHidden || !layer.value("visible", true);
            if (layer.value("guide", false)) continue;
            if (titleart::layerTypeOf(layer) == titleart::LayerType::Group) {
                leaves(layer["children"], path + "/", hidden, out, names);
                continue;
            }
            if (titleart::layerTypeOf(layer) == titleart::LayerType::Adjustment) continue;
            TitlePart p;
            p.layerId = str(layer, "id");
            p.path = path;
            p.name = name;
            p.blend = str(layer, "blend").empty() ? "normal" : str(layer, "blend");
            p.opacity = layer.value("opacity", 1.0);
            p.hidden = hidden;
            std::string base = slug(name);
            std::string unique = base;
            for (int k = 2; names.count(unique); k++) unique = base + "_" + std::to_string(k);
            names.insert(unique);
            p.fileStem = unique;
            out.push_back(std::move(p));
        }
    }

}

std::vector<TitlePart> editor::titleExportParts(const titleart::TitleArtDoc& doc, bool splitTitles) {
    std::vector<TitlePart> base;
    std::set<std::string> names;
    leaves(doc.root["layers"], "", false, base, names);
    std::vector<TitlePart> out;
    for (TitlePart& p : base) {
        const Json* layer = titleart::findLayer(doc.root, p.layerId);
        const bool title = layer && splitTitles && titleart::layerTypeHasStyle(titleart::layerTypeOf(*layer)) &&
                           str(*layer, "dimension") != "3d";
        if (!title) {
            out.push_back(p);
            continue;
        }
        // rim N alone (outermost first), the face, then fx: the whole layer, the one shown
        std::vector<TitlePart> rims;
        const Json& list = (*layer)["style"]["rims"];
        int number = 0;
        for (size_t i = 0; i < list.size(); i++) {
            const Json& rim = list[i];
            number++;
            if (!rim.value("enabled", true) || !(rim.value("width", 0.0) > 0)) continue;
            TitlePart r = p;
            r.part = "rim_" + std::to_string(number);
            r.rimIndex = (int)i;
            r.hidden = true;
            rims.push_back(r);
        }
        std::reverse(rims.begin(), rims.end());
        for (TitlePart& r : rims) out.push_back(std::move(r));
        TitlePart face = p;
        face.part = "face";
        face.hidden = true;
        out.push_back(face);
        TitlePart fx = p;
        fx.part = "fx";
        out.push_back(fx);
    }
    for (size_t i = 0; i < out.size(); i++) out[i].order = (int)i;
    return out;
}

titleart::TitleArtDoc editor::titleExportPartDoc(const titleart::TitleArtDoc& doc, const TitlePart& part) {
    titleart::TitleArtDoc copy = doc;
    Json* layer = findMutable(copy.root["layers"], part.layerId);
    if (!layer) return copy;
    // the part is drawn at full strength and normal blend: the manifest carries them
    (*layer)["opacity"] = 1.0;
    (*layer)["blend"] = "normal";
    (*layer)["visible"] = true;
    if (part.part.empty() || part.part == "fx") return copy;
    Json& style = (*layer)["style"];
    Json& parts = (*layer)["part_effects"];
    (*layer)["effects"] = Json::array();
    if (part.part == "face") {
        for (Json& rim : style["rims"]) rim["enabled"] = false;
        Json keep = Json::object();
        if (parts.is_object() && parts.contains("face")) keep["face"] = parts["face"];
        parts = keep;
        return copy;
    }
    // rim N: alone, the face hidden, its width the cumulative width of the rims up to it
    double grown = 0;
    Json& rims = style["rims"];
    std::string rimId;
    for (size_t i = 0; i < rims.size(); i++) {
        Json& rim = rims[i];
        const bool counts = rim.value("enabled", true) && rim.value("width", 0.0) > 0;
        if ((int)i <= part.rimIndex && counts) grown += rim.value("width", 0.0);
        if ((int)i == part.rimIndex) {
            rim["width"] = grown;
            rimId = str(rim, "id");
        } else {
            rim["enabled"] = false;
        }
    }
    style["face"]["opacity"] = 0.0;
    Json keep = Json::object();
    if (parts.is_object()) {
        for (auto it = parts.begin(); it != parts.end(); ++it) {
            if (it.key() == "rim:" + rimId) keep[it.key()] = it.value();
        }
    }
    parts = keep;
    return copy;
}

namespace {

    struct LanguageState {
        std::string code;                   // "" for the source alone
        std::shared_ptr<const titleart::TitleArtDoc> doc;
        TitleRectI trimmed;
        TitleRectI frame;
        double scale = 1.0;
        int width = 0, height = 0;          // the output size (round_to, pot)
    };

    bool planPng(TitleExportContext& ctx) {
        const bool parts = ctx.request.options.value("parts", false);
        const bool split = ctx.request.options.value("split_titles", false);
        for (const char* key : {"parts", "split_titles"}) {
            if (ctx.request.options.contains(key) && !ctx.request.options[key].is_boolean()) {
                return ctx.fail("bad_option", std::string(key) + " is true or false");
            }
        }
        if (split && !parts) return ctx.fail("bad_option", "split_titles splits the parts: pass parts true with it");
        auto states = std::make_shared<std::vector<LanguageState>>();
        if (ctx.request.languages.empty()) {
            states->push_back({"", nullptr, {}, {}, 1.0, 0, 0});
        } else {
            for (const std::string& code : ctx.request.languages) states->push_back({code, nullptr, {}, {}, 1.0, 0, 0});
        }
        for (size_t i = 0; i < states->size(); i++) {
            (*states)[i].doc = std::make_shared<titleart::TitleArtDoc>(ctx.resolved((*states)[i].code));
            ctx.measure((*states)[i].doc, "", "", [states, i](TitleRenderedImage&, const TitleRectI& frame) { (*states)[i].trimmed = frame; });
        }
        TitleExportContext* c = &ctx;
        ctx.then([c, states, parts, split] {
            const bool registered = c->request.options.value("registered", false);
            TitleRectI shared;
            for (const LanguageState& s : *states) shared = titleart::unionRect(shared, s.trimmed);
            for (LanguageState& s : *states) {
                if (s.trimmed.empty() && !registered) c->warn("output_empty: " + (s.code.empty() ? std::string("the title") : s.code) + " shows nothing");
                s.scale = c->image.scale;
                s.frame = c->finishFrame(registered ? shared : s.trimmed, s.scale, &s.width, &s.height);
                c->result.frames[s.code.empty() ? titleart::titleSourceLanguage(c->request.doc.root) : s.code] =
                    TitleJson::array({s.frame.x, s.frame.y, s.frame.width, s.frame.height});
                const std::string code = s.code;
                if (!parts) {
                    c->renderFrame(s.doc, "", s.frame, s.scale, "", [c, code](texsynth::Image& pixels) {
                        std::vector<uint8_t> png;
                        if (!c->encodePng(pixels, code, png)) return;
                        c->write(c->fileName("", code, "png"), std::string(png.begin(), png.end()));
                    }, s.width, s.height);
                    continue;
                }
                const std::vector<TitlePart> list = titleExportParts(*s.doc, split);
                auto manifest = std::make_shared<TitleJson>(TitleJson::array());
                for (const TitlePart& part : list) {
                    auto partDoc = std::make_shared<titleart::TitleArtDoc>(titleExportPartDoc(*s.doc, part));
                    const std::string file = c->fileName(part.fileStem + (part.part.empty() ? "" : "_" + part.part), code, "png");
                    c->renderFrame(partDoc, "", s.frame, s.scale, part.layerId, [c, code, file, part, manifest](texsynth::Image& pixels) {
                        std::vector<uint8_t> png;
                        if (!c->encodePng(pixels, code, png)) return;
                        if (!c->write(file, std::string(png.begin(), png.end()))) return;
                        manifest->push_back(TitleJson{{"file", file}, {"name", part.name}, {"layer", part.layerId}, {"path", part.path},
                                                      {"part", part.part}, {"blend", part.blend}, {"opacity", part.opacity},
                                                      {"order", part.order}, {"hidden", part.hidden}});
                    }, s.width, s.height);
                }
                const TitleRectI frame = s.frame;
                c->then([c, code, frame, manifest] {
                    TitleJson j = TitleJson::object();
                    j["format"] = "jasperengine.titleart.parts";
                    j["version"] = 1;
                    j["frame"] = TitleJson::array({frame.x, frame.y, frame.width, frame.height});
                    j["language"] = code.empty() ? titleart::titleSourceLanguage(c->request.doc.root) : code;
                    j["parts"] = *manifest;
                    std::string name = c->fileName("", code, "parts.json");
                    return c->write(name, j.dump(2) + "\n");
                });
            }
            return true;
        });
        return true;
    }

}

void editor::registerTitlePngExport() {
    TitleExportFormat png;
    png.name = "png";
    png.optionKeys = {"parts", "split_titles"};
    png.plan = planPng;
    registerTitleExportFormat(std::move(png));
}
