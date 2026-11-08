// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtBakeInfo.h"

#include "io/Data.h"
#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>

using namespace jasperengine;

namespace {

    using Json = nlohmann::json;

    double number(const Json& object, const char* key, double fallback){
        auto it = object.find(key);
        return it != object.end() && it->is_number() && std::isfinite(it->get<double>()) ? it->get<double>() : fallback;
    }

    std::string text(const Json& object, const char* key){
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    TitleArtRectI rect(const Json& object, const char* key){
        TitleArtRectI out;
        auto it = object.find(key);
        if (it != object.end() && it->is_array() && it->size() == 4){
            const Json& a = *it;
            for (const Json& v : a) if (!v.is_number()) return out;
            out = {(int)a[0].get<double>(), (int)a[1].get<double>(), std::max(0, (int)a[2].get<double>()), std::max(0, (int)a[3].get<double>())};
            return out;
        }
        if (it == object.end() || !it->is_object()) return out;
        out.x = (int)std::lround(number(*it, "x", 0.0));
        out.y = (int)std::lround(number(*it, "y", 0.0));
        out.width = std::max(0, (int)std::lround(number(*it, "w", number(*it, "width", 0.0))));
        out.height = std::max(0, (int)std::lround(number(*it, "h", number(*it, "height", 0.0))));
        return out;
    }

    float unit(double value){
        return (float)std::min(1.0, std::max(0.0, value));
    }

}

bool TitleArtBakeInfo::parse(const std::string& source, TitleArtBakeInfo& out, std::string& error){
    Json json = Json::parse(source, nullptr, false);
    if (json.is_discarded() || !json.is_object() || text(json, "format") != TITLE_ART_BAKEINFO_FORMAT_NAME){
        error = std::string("not a Title Art bake sidecar (format ") + TITLE_ART_BAKEINFO_FORMAT_NAME + ")";
        return false;
    }
    const int version = (int)number(json, "version", 0.0);
    if (version > TITLE_ART_BAKEINFO_READ_VERSION){
        error = "the bake sidecar is version " + std::to_string(version) + ", written by a newer editor; this build reads version " +
                std::to_string(TITLE_ART_BAKEINFO_READ_VERSION) + ": bake it again with this editor";
        return false;
    }
    out = TitleArtBakeInfo();
    out.frame = rect(json, "frame");
    auto origin = json.find("origin");
    if (origin != json.end() && origin->is_array() && origin->size() == 2){
        out.originX = (*origin)[0].is_number() ? (*origin)[0].get<double>() : 0.0;
        out.originY = (*origin)[1].is_number() ? (*origin)[1].get<double>() : 0.0;
    }else{
        out.originX = out.frame.width * 0.5;
        out.originY = out.frame.height * 0.5;
    }
    auto outputs = json.find("outputs");
    if (outputs != json.end() && outputs->is_array()){
        for (const Json& item : *outputs){
            if (!item.is_object()) continue;
            TitleArtBakeOutput o;
            o.id = text(item, "id");
            o.file = text(item, "file");
            o.content = text(item, "content");
            auto size = item.find("size");
            if (size != item.end() && size->is_array() && size->size() == 2){
                o.width = (*size)[0].is_number() ? (*size)[0].get<int>() : 0;
                o.height = (*size)[1].is_number() ? (*size)[1].get<int>() : 0;
            }
            if (!o.file.empty()) out.outputs.push_back(std::move(o));
        }
    }
    auto anchors = json.find("anchors");
    if (anchors != json.end() && anchors->is_array()){
        for (const Json& item : *anchors){
            if (!item.is_object()) continue;
            TitleArtAnchor a;
            a.x = unit(number(item, "x", 0.0));
            a.y = unit(number(item, "y", 0.0));
            a.size = (float)std::max(0.0, number(item, "size", 0.0));
            a.strength = unit(number(item, "strength", 1.0));
            a.fade = unit(number(item, "fade", 1.0));
            out.anchors.push_back(a);
        }
        // the strongest first (a sidecar may list them in any order), at most 16 (R36)
        std::stable_sort(out.anchors.begin(), out.anchors.end(), [](const TitleArtAnchor& a, const TitleArtAnchor& b){
            return a.strength * a.fade > b.strength * b.fade;
        });
        if (out.anchors.size() > TITLE_ART_MAX_ANCHORS) out.anchors.resize(TITLE_ART_MAX_ANCHORS);
    }
    out.alphaBounds = rect(json, "alpha_bounds");
    out.sourceHash = text(json, "source_hash");
    out.bakeVersion = (uint32_t)std::max(0.0, number(json, "bake_version", 0.0));
    out.language = text(json, "language");
    return true;
}

bool TitleArtBakeInfo::load(const std::string& path, TitleArtBakeInfo& out, std::string& error){
    Data data;
    if (data.open(path.c_str()) != FILEDATA_OK){
        error = "cannot read " + path;
        return false;
    }
    const std::string source(reinterpret_cast<const char*>(data.getMemPtr()), data.length());
    return parse(source, out, error);
}

const TitleArtBakeOutput* TitleArtBakeInfo::firstOutput(const std::string& content) const{
    for (const TitleArtBakeOutput& o : outputs)
        if (o.content == content) return &o;
    return nullptr;
}

const TitleArtBakeOutput* TitleArtBakeInfo::output(const std::string& id) const{
    for (const TitleArtBakeOutput& o : outputs)
        if (o.id == id) return &o;
    return nullptr;
}

float TitleArtBakeInfo::pivotX() const{
    return frame.width > 0 ? (float)(originX / frame.width) : 0.5f;
}

float TitleArtBakeInfo::pivotY() const{
    return frame.height > 0 ? (float)(originY / frame.height) : 0.5f;
}

std::string jasperengine::titleArtSidecarPath(const std::string& titlePath){
    return titlePath + TITLE_ART_BAKEINFO_SUFFIX;
}

std::string jasperengine::titleArtSiblingPath(const std::string& titlePath, const std::string& file){
    const size_t slash = titlePath.find_last_of('/');
    return slash == std::string::npos ? file : titlePath.substr(0, slash + 1) + file;
}

std::string jasperengine::titleArtPrebakePath(const std::string& titlePath, uint64_t stateHash){
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)stateHash);
    return titlePath + ".prebake-" + hex + ".png";
}

std::string jasperengine::titleArtGlyphPrebakePath(const std::string& titlePath, uint64_t styleHash){
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)styleHash);
    return titlePath + ".glyphs-" + hex + ".json";
}

std::string jasperengine::titleArtGlyphPrebakeFile(const std::string& manifestPath, const char* kind, int index){
    const std::string base = manifestPath.size() > 5 ? manifestPath.substr(0, manifestPath.size() - 5) : manifestPath;
    return base + "-" + kind + std::to_string(index) + ".png";
}

std::string jasperengine::titleArtDefaultBakeFile(const std::string& titlePath){
    const size_t slash = titlePath.find_last_of('/');
    const std::string name = slash == std::string::npos ? titlePath : titlePath.substr(slash + 1);
    return name + ".png";
}
