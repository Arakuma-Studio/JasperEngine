// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ColorScheme.h"

#include "TitleArtIO.h"
#include "TitleArtSchema.h"

#include <algorithm>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const Json NULL_JSON;

    // ---- schema defaults (cached: the writer compares every stop and material against them) ----

    const Json& kindDefault(const std::string& kind) {
        static std::mutex mutex;
        static std::map<std::string, Json> cache;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(kind);
        if (it == cache.end()) it = cache.emplace(kind, TitleArtSchema::get().defaultObject(kind)).first;
        return it->second;
    }

    bool hasKind(const char* kind) {
        return TitleArtSchema::get().kind(kind) != nullptr;
    }

    // ---- lenient reads ----

    struct Reader {
        std::vector<std::string>* warnings = nullptr;

        void warn(const std::string& path, const std::string& message) const {
            if (warnings) warnings->push_back(path + ": " + message);
        }

        // `value` as the complete object of `kind`; a non-object takes the kind's defaults
        Json object(const Json& value, const char* kind, const std::string& path) const {
            Json out = value;
            if (!out.is_object()) {
                if (!value.is_null()) warn(path, "expected an object: the default is used");
                out = Json::object();
            }
            // without the 3D module `material_3d` is no kind: the object is kept as it is
            if (!hasKind(kind)) return out;
            std::vector<TitleWarning> notes;
            NormalizeContext context;
            context.warnings = &notes;
            normalizeTitleArtObject(out, kind, path, context);
            for (const TitleWarning& w : notes) warn(w.path.empty() ? path : w.path, w.message);
            return out;
        }

        // an object of `kind` or null
        Json nullable(const Json& value, const char* kind, const std::string& path) const {
            if (value.is_null()) return nullptr;
            if (!value.is_object()) {
                warn(path, "expected an object or null: null is used");
                return nullptr;
            }
            return object(value, kind, path);
        }

        std::string color(const Json& value, const std::string& fallback, const std::string& path) const {
            TitleColor c;
            if (parseTitleColor(value, c)) return formatTitleColor(c);
            warn(path, "expected a colour: " + fallback + " is used");
            return fallback;
        }

        ColorPaint paint(const Json& value, const std::string& path) const {
            ColorPaint p;
            if (!value.is_object()) {
                warn(path, "expected a paint object: the default is used");
                p.fill = object(Json::object(), "fill", jsonPathMember(path, "fill"));
                p.material3d = object(Json::object(), "material_3d", jsonPathMember(path, "material_3d"));
                return p;
            }
            auto get = [&](const char* key) -> const Json& {
                auto it = value.find(key);
                return it != value.end() ? *it : NULL_JSON;
            };
            p.fill = object(get("fill"), "fill", jsonPathMember(path, "fill"));
            p.ramp = nullable(get("ramp"), "gradient", jsonPathMember(path, "ramp"));
            p.material3d = object(get("material_3d"), "material_3d", jsonPathMember(path, "material_3d"));
            if (value.contains("shade")) p.shade = color(get("shade"), "#000000", jsonPathMember(path, "shade"));
            p.pattern = nullable(get("pattern"), "pattern", jsonPathMember(path, "pattern"));
            for (auto it = value.begin(); it != value.end(); ++it) {
                const std::string& key = it.key();
                if (key != "fill" && key != "ramp" && key != "material_3d" && key != "shade" && key != "pattern") p.extra[key] = it.value();
            }
            return p;
        }
    };

    // ---- sparse writing ----

    Json sparseAgainst(const Json& value, const Json& defaults, const SchemaKind* kind) {
        if (!kind || !value.is_object()) return value;
        const TitleArtSchema& schema = TitleArtSchema::get();
        Json out = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string& key = it.key();
            const Json& v = it.value();
            const SchemaMember* m = schema.member(*kind, key);
            if (!m) {
                out[key] = v;
                continue;
            }
            auto d = defaults.is_object() ? defaults.find(key) : defaults.end();
            const bool hasDefault = defaults.is_object() && d != defaults.end();
            if (hasDefault && *d == v) continue;
            if (m->type == ValueType::Object && v.is_object()) {
                const SchemaKind* child = schema.kind(m->kind);
                const Json& base = hasDefault && d->is_object() ? *d : kindDefault(m->kind);
                out[key] = sparseAgainst(v, base, child);
            } else if (m->type == ValueType::ObjectArray && v.is_array()) {
                const SchemaKind* child = schema.kind(m->kind);
                Json list = Json::array();
                for (const Json& item : v) list.push_back(item.is_object() && child ? sparseAgainst(item, kindDefault(m->kind), child) : item);
                out[key] = std::move(list);
            } else {
                out[key] = v;
            }
        }
        return out;
    }

    Json sparsePaint(const ColorPaint& p) {
        Json out = Json::object();
        if (!p.fill.is_null()) {
            Json fill = sparseSchemaObject(p.fill, "fill");
            if (!fill.empty()) out["fill"] = std::move(fill);
        }
        if (!p.ramp.is_null()) out["ramp"] = sparseSchemaObject(p.ramp, "gradient");
        if (!p.material3d.is_null()) {
            Json material = sparseSchemaObject(p.material3d, "material_3d");
            if (!material.empty()) out["material_3d"] = std::move(material);
        }
        if (p.shade != "#000000") out["shade"] = p.shade;
        if (!p.pattern.is_null()) out["pattern"] = sparseSchemaObject(p.pattern, "pattern");
        for (auto it = p.extra.begin(); it != p.extra.end(); ++it) out[it.key()] = it.value();
        return out;
    }

    // a gradient member: omitted when null or equal to the schema's default
    void putGradient(Json& out, const char* key, const Json& gradient) {
        if (gradient.is_null()) return;
        Json sparse = sparseSchemaObject(gradient, "gradient");
        if (!sparse.empty()) out[key] = std::move(sparse);
    }

    // ---- the document member ----

    // color_schemes: an object of scheme objects, keys sorted (entries are kept as written: a
    // scheme of a newer version stays for the build that reads it)
    void normalizeEmbeddedSchemes(Json& value, const std::string& path, NormalizeContext& context) {
        if (!value.is_object()) {
            context.warn(path, "expected an object of color schemes: {} is used");
            value = Json::object();
            return;
        }
        std::vector<std::string> keys;
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.value().is_object() && !it.key().empty()) keys.push_back(it.key());
            else context.warn(jsonPathMember(path, it.key()), "not a color scheme object: dropped");
        }
        std::sort(keys.begin(), keys.end());
        Json out = Json::object();
        for (const std::string& key : keys) out[key] = std::move(value[key]);
        value = std::move(out);
    }

    void (*previousDocumentFinish)(Json&, const std::string&, NormalizeContext&) = nullptr;

    // a document without embedded schemes does not write the member, so documents saved before
    // S84 keep their bytes
    void finishDocument(Json& root, const std::string& path, NormalizeContext& context) {
        if (previousDocumentFinish) previousDocumentFinish(root, path, context);
        auto it = root.find("color_schemes");
        if (it != root.end() && it->is_object() && it->empty()) root.erase(it);
    }

}

TitleJsonMigrations& titleart::titleColorsMigrations() {
    static TitleJsonMigrations* table = new TitleJsonMigrations(TITLE_COLORS_FORMAT, 1);
    return *table;
}

Json titleart::sparseSchemaObject(const Json& full, const std::string& kind, const Json& defaults) {
    const SchemaKind* k = TitleArtSchema::get().kind(kind);
    if (!k) return full;
    return sparseAgainst(full, defaults.is_null() ? kindDefault(kind) : defaults, k);
}

bool titleart::parseColorScheme(const Json& j, ColorScheme& out, std::vector<std::string>* warnings, std::string* errorCode) {
    auto fail = [&](const char* code, const std::string& message) {
        if (errorCode) *errorCode = code;
        if (warnings) warnings->push_back(std::string(code) + ": " + message);
        return false;
    };
    if (!j.is_object()) return fail("not_object", "a color scheme is a JSON object");
    auto format = j.find("format");
    if (format == j.end() || !format->is_string() || format->get<std::string>() != TITLE_COLORS_FORMAT) {
        return fail("bad_format", std::string("not a color scheme: \"format\" must be \"") + TITLE_COLORS_FORMAT + "\"");
    }
    auto version = j.find("version");
    if (version == j.end() || !version->is_number_integer() || version->get<int64_t>() < 1) {
        return fail("bad_version", "\"version\" must be a whole number from 1");
    }
    const int read = (int)version->get<int64_t>();
    if (read > TITLE_COLORS_VERSION) {
        return fail("version_newer", "this color scheme is version " + std::to_string(read) + ", written by a newer build; this build reads up to version " +
                                         std::to_string(TITLE_COLORS_VERSION));
    }
    Json root = j;
    if (read < TITLE_COLORS_VERSION) {
        MigrationLog log;
        std::string error;
        if (!titleColorsMigrations().run(root, read, TITLE_COLORS_VERSION, log, &error)) return fail("migration_failed", error);
        if (warnings) for (const std::string& note : log.notes) warnings->push_back(note);
    }

    Reader r{warnings};
    ColorScheme s;
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string& key = it.key();
        const Json& v = it.value();
        if (key == "format" || key == "version") continue;
        if (key == "id") {
            if (v.is_string()) s.id = v.get<std::string>();
            else r.warn(key, "expected a string: \"\" is used");
        } else if (key == "revision") {
            if (v.is_number_integer() && v.get<int64_t>() >= 1) s.revision = (int)v.get<int64_t>();
            else r.warn(key, "expected a whole number from 1: 1 is used");
        } else if (key == "name") {
            s.name = v;
        } else if (key == "description") {
            s.description = v;
        } else if (key == "family_hint") {
            s.familyHint = v;
        } else if (key == "tags") {
            s.tags = v;
        } else if (key == "synonyms") {
            s.synonyms = v;
        } else if (key == "swatch") {
            s.swatch = v;
        } else if (key == "provenance") {
            s.provenance = v;
        } else if (key == "paints") {
            if (!v.is_object()) {
                r.warn(key, "expected an object: the default paints are used");
                continue;
            }
            for (auto p = v.begin(); p != v.end(); ++p) {
                const std::string at = jsonPathMember("paints", p.key());
                if (p.key() == "face") s.face = r.paint(p.value(), at);
                else if (p.key() == "dark") s.dark = r.paint(p.value(), at);
                else if (p.key() == "base") s.base = r.paint(p.value(), at);
                else if (p.key() == "accents") {
                    if (!p.value().is_array()) {
                        r.warn(at, "expected a list of paints: none is used");
                        continue;
                    }
                    for (size_t i = 0; i < p.value().size(); i++) s.accents.push_back(r.paint(p.value()[i], jsonPathIndex(at, i)));
                } else if (p.key() == "bevel_rim_3d") {
                    if (!p.value().is_null()) s.bevelRim3d = r.paint(p.value(), at);
                } else if (p.key() == "side_3d") {
                    s.side3d = r.object(p.value(), "material_3d", at);
                } else {
                    s.paintsExtra[p.key()] = p.value();
                }
            }
        } else if (key == "face_glow") {
            s.faceGlow = r.color(v, "#ffffff", key);
        } else if (key == "glow") {
            s.glow = r.color(v, "#ffffff", key);
        } else if (key == "glow_gradient") {
            s.glowGradient = r.object(v, "gradient", key);
        } else if (key == "shadow") {
            s.shadow = r.color(v, "#000000", key);
        } else if (key == "glint") {
            s.glint = r.color(v, "#ffffff", key);
        } else if (key == "light_ramp") {
            if (!v.is_array()) {
                r.warn(key, "expected a list of colours: none is used");
                continue;
            }
            for (size_t i = 0; i < v.size(); i++) {
                TitleColor c;
                if (parseTitleColor(v[i], c)) s.lightRamp.push_back(formatTitleColor(c));
                else r.warn(jsonPathIndex(key, i), "not a colour: dropped");
            }
            if (s.lightRamp.size() < 4 || s.lightRamp.size() > 7) {
                r.warn(key, std::to_string(s.lightRamp.size()) + " colours: a light ramp has 4 to 7");
            }
        } else if (key == "orb") {
            if (!v.is_object()) {
                r.warn(key, "expected an object: the default is used");
                continue;
            }
            for (auto o = v.begin(); o != v.end(); ++o) {
                if (o.key() == "ramp") s.orb.ramp = r.object(o.value(), "gradient", "orb.ramp");
                else if (o.key() == "radial") {
                    if (o.value().is_boolean()) s.orb.radial = o.value().get<bool>();
                    else r.warn("orb.radial", "expected true or false: true is used");
                } else s.orb.extra[o.key()] = o.value();
            }
        } else if (key == "pattern") {
            s.pattern = r.nullable(v, "pattern", key);
        } else if (key == "pattern_ground") {
            if (!v.is_null()) s.patternGround = r.color(v, "#ffc81e", key);
        } else if (key == "spectrum") {
            if (v.is_boolean()) s.spectrum = v.get<bool>();
            else r.warn(key, "expected true or false: false is used");
        } else {
            s.extra[key] = v;
        }
    }
    // members a file omits are at their defaults: complete them, as the payloads' contract says
    auto complete = [&](ColorPaint& p, const std::string& at) {
        if (p.fill.is_null()) p.fill = r.object(Json::object(), "fill", jsonPathMember(at, "fill"));
        if (p.material3d.is_null()) p.material3d = r.object(Json::object(), "material_3d", jsonPathMember(at, "material_3d"));
    };
    complete(s.face, "paints.face");
    complete(s.dark, "paints.dark");
    complete(s.base, "paints.base");
    for (ColorPaint& p : s.accents) complete(p, "paints.accents");
    if (s.bevelRim3d) complete(*s.bevelRim3d, "paints.bevel_rim_3d");
    if (s.side3d.is_null()) s.side3d = r.object(Json::object(), "material_3d", "paints.side_3d");
    if (s.glowGradient.is_null()) s.glowGradient = r.object(Json::object(), "gradient", "glow_gradient");
    if (s.orb.ramp.is_null()) s.orb.ramp = r.object(Json::object(), "gradient", "orb.ramp");
    out = std::move(s);
    return true;
}

Json titleart::writeColorScheme(const ColorScheme& s) {
    Json out = Json::object();
    out["format"] = TITLE_COLORS_FORMAT;
    out["version"] = TITLE_COLORS_VERSION;
    out["id"] = s.id;
    out["revision"] = s.revision;
    if (!s.name.is_null()) out["name"] = s.name;
    if (!s.description.is_null()) out["description"] = s.description;
    if (!s.familyHint.is_null()) out["family_hint"] = s.familyHint;
    if (!s.tags.is_null()) out["tags"] = s.tags;
    if (!s.synonyms.is_null()) out["synonyms"] = s.synonyms;

    Json paints = Json::object();
    auto putPaint = [&](const char* key, const ColorPaint& p) {
        Json sparse = sparsePaint(p);
        if (!sparse.empty()) paints[key] = std::move(sparse);
    };
    putPaint("face", s.face);
    putPaint("dark", s.dark);
    if (!s.accents.empty()) {
        Json list = Json::array();
        for (const ColorPaint& p : s.accents) list.push_back(sparsePaint(p));
        paints["accents"] = std::move(list);
    }
    putPaint("base", s.base);
    if (s.bevelRim3d) paints["bevel_rim_3d"] = sparsePaint(*s.bevelRim3d);
    if (!s.side3d.is_null()) {
        Json side = sparseSchemaObject(s.side3d, "material_3d");
        if (!side.empty()) paints["side_3d"] = std::move(side);
    }
    for (auto it = s.paintsExtra.begin(); it != s.paintsExtra.end(); ++it) paints[it.key()] = it.value();
    out["paints"] = std::move(paints);

    if (s.faceGlow != "#ffffff") out["face_glow"] = s.faceGlow;
    if (s.glow != "#ffffff") out["glow"] = s.glow;
    putGradient(out, "glow_gradient", s.glowGradient);
    if (s.shadow != "#000000") out["shadow"] = s.shadow;
    if (s.glint != "#ffffff") out["glint"] = s.glint;
    if (!s.lightRamp.empty()) out["light_ramp"] = s.lightRamp;
    Json orb = Json::object();
    putGradient(orb, "ramp", s.orb.ramp);
    if (!s.orb.radial) orb["radial"] = false;
    for (auto it = s.orb.extra.begin(); it != s.orb.extra.end(); ++it) orb[it.key()] = it.value();
    if (!orb.empty()) out["orb"] = std::move(orb);
    if (!s.pattern.is_null()) out["pattern"] = sparseSchemaObject(s.pattern, "pattern");
    if (s.patternGround) out["pattern_ground"] = *s.patternGround;
    if (s.spectrum) out["spectrum"] = true;
    if (!s.swatch.is_null()) out["swatch"] = s.swatch;
    if (!s.provenance.is_null()) out["provenance"] = s.provenance;
    for (auto it = s.extra.begin(); it != s.extra.end(); ++it) {
        if (!out.contains(it.key())) out[it.key()] = it.value();
    }
    return out;
}

std::string titleart::writeColorSchemeText(const ColorScheme& s) {
    return writeTitleArtJson(writeColorScheme(s)) + "\n";
}

void titleart::registerColorSchemeMembers(TitleArtSchema& schema) {
    SchemaMember m;
    m.key = "color_schemes";
    m.type = ValueType::Json;
    m.defaultValue = Json::object();
    m.label = "Color Schemes";
    m.anim = FxAnim::None;
    m.interp = ParamInterp::Stepped;
    // a player renders the document with one of them applied: the embedded data itself draws nothing
    m.hash = HashScope::Identity;
    m.owner = "S84";
    m.normalize = &normalizeEmbeddedSchemes;
    if (!schema.addMember("document", std::move(m))) return;
    // the schema has no per-member "omit when default" for a full kind; the document's finish
    // step drops the member when it is empty (the kind is owned by the schema, never const)
    if (const SchemaKind* document = schema.kind("document")) {
        SchemaKind* kind = const_cast<SchemaKind*>(document);
        previousDocumentFinish = kind->finish;
        kind->finish = &finishDocument;
    }
}
