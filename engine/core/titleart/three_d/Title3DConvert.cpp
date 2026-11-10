// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Title3DConvert.h"

#include <cctype>
#include <string>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    std::string snake(const std::string& camel) {
        std::string out;
        for (char c : camel) {
            if (std::isupper((unsigned char)c)) {
                out += '_';
                out += (char)std::tolower((unsigned char)c);
            } else {
                out += c;
            }
        }
        return out;
    }

    const Json& at(const Json& o, const char* key) {
        static const Json none;
        if (!o.is_object()) return none;
        auto it = o.find(key);
        return it != o.end() ? *it : none;
    }

    // copies `from[key]` to `to[name]` when present
    void copy(const Json& from, const char* key, Json& to, const char* name = nullptr) {
        const Json& v = at(from, key);
        if (!v.is_null()) to[name ? name : snake(key)] = v;
    }

    Json environmentOf(const Json& e) {
        Json out = Json::object();
        if (!e.is_object()) return out;
        for (const char* key : {"preset", "intensity", "rotation", "tint", "contrast", "saturation", "blur", "repeatX", "repeatY"}) copy(e, key, out);
        return out;
    }

    Json bevelOf(const Json& b) {
        Json out = Json::object();
        for (const char* key : {"width", "depth", "profile", "segments", "curve"}) copy(b, key, out);
        return out;
    }

    Json patternOf(const Json& p) {
        Json out = Json::object();
        for (const char* key : {"kind", "color", "color2", "opacity", "scale", "angle", "seed", "shade", "relief"}) copy(p, key, out);
        return out;
    }

}

Json titleart::material3DFromPachislop(const Json& m) {
    Json out = Json::object();
    if (!m.is_object()) return out;
    static const char* const PRESETS[] = {"custom", "gold", "punchy_gold", "pale_gold", "rose_gold", "chrome", "silver", "copper",
                                          "red_metal", "blue_metal", "green_metal", "purple_metal", "black_gloss", "white_gloss", "rainbow",
                                          "glass", "plastic", "emissive", "candy", "diamond", "ruby", "sapphire", "emerald"};
    std::string preset = at(m, "preset").is_string() ? snake(at(m, "preset").get<std::string>()) : "custom";
    bool known = false;
    for (const char* p : PRESETS) known = known || preset == p;
    out["preset"] = known ? preset : "custom";
    for (const char* key : {"color", "gradient", "gradientKind", "gradientAngle", "gradientSpan", "gradientScale", "gradientOffset", "gradientAxis",
                            "gradientBend", "roughness", "clearcoat", "clearcoatRoughness", "iridescence", "iridescenceIor", "transmission", "ior",
                            "dispersion", "glassDepth", "flakes", "flakeScale", "opacity"}) {
        copy(m, key, out);
    }
    copy(m, "metalness", out, "metallic");
    copy(m, "reflectivity", out, "env_multiplier");
    copy(m, "specBoost", out, "specular_multiplier");
    copy(m, "emissive", out, "emission");
    copy(m, "emissiveIntensity", out, "emission_intensity");
    if (at(m, "pattern").is_object()) out["pattern"] = patternOf(at(m, "pattern"));
    if (at(m, "surface").is_object()) {
        Json s = Json::object();
        for (const char* key : {"kind", "amount", "scale", "seed"}) copy(at(m, "surface"), key, s);
        out["surface"] = s;
    }
    if (at(m, "environment").is_object()) out["environment"] = environmentOf(at(m, "environment"));
    return out;
}

Json titleart::title3DFromPachislop(const Json& p) {
    Json out = Json::object();
    if (!p.is_object()) return out;
    for (const char* key : {"mode", "depth", "curveSegments", "bevelPosition", "faceImage", "edgeImage", "envLock"}) copy(p, key, out);
    if (at(p, "skew").is_object()) out["skew"] = Json::array({at(p, "skew").value("x", 0.0), at(p, "skew").value("y", 0.0)});
    if (at(p, "bevel").is_object()) out["bevel"] = bevelOf(at(p, "bevel"));
    if (at(p, "inflate").is_object()) {
        Json inflate = Json::object();
        for (const char* key : {"height", "profile", "smoothness", "detail"}) copy(at(p, "inflate"), key, inflate);
        out["inflate"] = inflate;
    }
    if (at(p, "face").is_object()) out["face"] = material3DFromPachislop(at(p, "face"));
    if (at(p, "side").is_object()) out["side"] = material3DFromPachislop(at(p, "side"));
    if (at(p, "bevelMaterial").is_object()) out["bevel_material"] = material3DFromPachislop(at(p, "bevelMaterial"));
    if (at(p, "bands").is_array()) {
        Json bands = Json::array();
        for (const Json& b : at(p, "bands")) {
            Json band = Json::object();
            for (const char* key : {"id", "enabled", "grow", "join", "fillCounters", "depth", "offset"}) copy(b, key, band);
            if (at(b, "shape").is_object()) {
                Json plate = Json::object();
                for (const char* key : {"kind", "close", "corner", "slant"}) copy(at(b, "shape"), key, plate);
                band["plate"] = plate;
            }
            if (at(b, "bevel").is_object()) band["bevel"] = bevelOf(at(b, "bevel"));
            if (at(b, "material").is_object()) band["material"] = material3DFromPachislop(at(b, "material"));
            if (at(b, "side").is_object()) band["side"] = material3DFromPachislop(at(b, "side"));
            bands.push_back(band);
        }
        out["bands"] = bands;
    }
    if (at(p, "glint").is_object()) {
        Json g = Json::object();
        for (const char* key : {"enabled", "threshold", "intensity", "radius", "streaks", "length", "angle", "tint"}) copy(at(p, "glint"), key, g);
        out["glint"] = g;
    }
    if (at(p, "ao").is_object()) {
        Json ao = Json::object();
        for (const char* key : {"enabled", "radius", "intensity"}) copy(at(p, "ao"), key, ao);
        out["ao"] = ao;
    }
    if (at(p, "lighting").is_object()) {
        const Json& l = at(p, "lighting");
        Json lighting = Json::object();
        lighting["environment"] = environmentOf(at(l, "environment"));
        if (at(l, "toneMapping").is_string()) {
            const std::string tone = at(l, "toneMapping").get<std::string>();
            lighting["tone_mapping"] = tone == "aces" ? "aces_filmic" : tone;
        }
        copy(l, "exposure", lighting);
        if (at(l, "lights").is_array()) {
            Json lights = Json::array();
            for (const Json& light : at(l, "lights")) {
                Json o = Json::object();
                for (const char* key : {"id", "type", "color", "intensity"}) copy(light, key, o);
                o["position"] = Json::array({light.value("x", 0.0), light.value("y", 0.0), light.value("z", 0.0)});
                lights.push_back(o);
            }
            lighting["lights"] = lights;
        }
        out["lighting"] = lighting;
    }
    return out;
}
