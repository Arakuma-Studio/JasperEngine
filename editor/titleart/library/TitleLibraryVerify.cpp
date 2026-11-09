// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibraryVerify.h"

#include "DeniedTerms.h"
#include "TitleLibraryApply.h"
#include "TitleLibraryImports.h"
#include "TitleLibraryProvider.h"
#include "TitleLibrarySearch.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"
#include "util/Sha256.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
namespace fs = std::filesystem;
using titleart::SchemaKind;
using titleart::SchemaMember;
using titleart::TitleArtSchema;
using titleart::ValueType;

namespace {

    std::string str(const Json& object, const char* key, const std::string& fallback = {}) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    bool readBytes(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    void unknownParams(const std::vector<titleart::ParamDef>& defs, const Json& params, const std::string& path, std::vector<std::string>& out) {
        if (!params.is_object()) return;
        for (auto it = params.begin(); it != params.end(); ++it) {
            const titleart::ParamDef* p = titleart::findParam(defs, it.key());
            if (!p) {
                out.push_back(path + "." + it.key());
                continue;
            }
            if (p->kind == titleart::ParamKind::RowArray && it.value().is_array()) {
                for (size_t i = 0; i < it.value().size(); i++) unknownParams(p->rows, it.value()[i], path + "." + it.key() + "[" + std::to_string(i) + "]", out);
            }
        }
    }

    void unknownEffects(const Json& list, const std::string& path, std::vector<std::string>& out) {
        if (!list.is_array()) return;
        for (size_t i = 0; i < list.size(); i++) {
            const Json& e = list[i];
            const std::string at = path + "[" + std::to_string(i) + "]";
            const titleart::EffectDef* def = titleart::findEffect(str(e, "type"));
            if (!def) continue;     // an unknown type is an availability matter, not a typo
            for (auto it = e.begin(); it != e.end(); ++it) {
                static const std::set<std::string> known = {"id", "type", "enabled", "params", "from_look"};
                if (!known.count(it.key())) out.push_back(at + "." + it.key());
            }
            if (e.contains("params")) unknownParams(def->params, e["params"], at + ".params", out);
        }
    }

}

void titlelibrary::unknownMembers(const Json& value, const std::string& kindName, const std::string& path, std::vector<std::string>& out) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    const SchemaKind* kind = schema.kind(kindName);
    if (!kind || !value.is_object()) return;
    const SchemaKind* variant = kind->discriminator.empty() ? kind : schema.variantOf(*kind, value);
    if (!variant) return;
    for (auto it = value.begin(); it != value.end(); ++it) {
        const std::string at = path.empty() ? it.key() : path + "." + it.key();
        const SchemaMember* m = schema.member(*variant, it.key());
        if (!m) {
            out.push_back(at);
            continue;
        }
        const Json& v = it.value();
        switch (m->type) {
            case ValueType::Object: unknownMembers(v, m->kind, at, out); break;
            case ValueType::ObjectArray:
                if (m->kind == "effect") unknownEffects(v, at, out);
                else if (v.is_array()) for (size_t i = 0; i < v.size(); i++) unknownMembers(v[i], m->kind, at + "[" + std::to_string(i) + "]", out);
                break;
            case ValueType::ObjectMap:
                if (v.is_object()) for (auto e = v.begin(); e != v.end(); ++e) unknownMembers(e.value(), m->kind, at + "." + e.key(), out);
                break;
            case ValueType::ObjectArrayMap:
                if (v.is_object()) for (auto e = v.begin(); e != v.end(); ++e) unknownEffects(e.value(), at + "." + e.key(), out);
                break;
            default: break;
        }
    }
}

// ---- thumbnails ----

std::vector<ThumbnailSample> titlelibrary::thumbnailSamples(const LibraryItemInfo& item) {
    if (item.kind == LibraryKind::Template) return {{"en", ""}, {"ja", ""}};
    const Json& m = TitleLibrary::get().manifest();
    const Json samples = m.contains("sample_texts") ? m["sample_texts"] : Json::object();
    std::vector<ThumbnailSample> out = {{"latin", str(samples, "latin", "HERO")}, {"kanji", str(samples, "kanji", "大当り")}, {"digits", str(samples, "digits", "1234")}};
    if (item.kind == LibraryKind::Look) {
        const Json j = TitleLibrary::get().itemJson(item);
        if (j.contains("thumbnail_texts") && j["thumbnail_texts"].is_object()) {
            for (ThumbnailSample& s : out) s.text = str(j["thumbnail_texts"], s.key.c_str(), s.text);
        }
    }
    return out;
}

bool titlelibrary::thumbnailDocument(const LibraryItemInfo& item, const ThumbnailSample& sample, titleart::TitleArtDoc& out,
                                     std::map<std::string, std::string>& transient, std::string& error) {
    titleart::NewTitleArtOptions o;
    o.text = sample.text.empty() ? std::string("HERO") : sample.text;
    titleart::TitleArtDoc doc = titleart::makeNewTitleArt(o);
    Json ops = Json::array();
    if (item.kind == LibraryKind::Look) {
        // a look with no font pair (effects-only) still shows its kanji sample: the pack's default face
        const Json raw = TitleLibrary::get().itemJson(item);
        if (!raw.contains("font") && findPackFont("dela-gothic-one")) {
            ops.push_back(Json{{"op", "set"}, {"layer", "Title"}, {"values", Json{{"font", Json{{"path", "TitleArt/Fonts/dela-gothic-one/DelaGothicOne-Regular.ttf"}, {"guid", ""}}}}}});
        }
        ops.push_back(Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", item.ref}, {"apply_font", true}, {"keep_color_scheme", false}});
    } else if (item.kind == LibraryKind::ColorScheme) {
        const std::string look = str(TitleLibrary::get().manifest(), "scheme_preview_look", "casino/outline-gold");
        ops.push_back(Json{{"op", "apply_look"}, {"layer", "Title"}, {"look", look}, {"apply_font", true}});
        ops.push_back(Json{{"op", "apply_color_scheme"}, {"layer", "Title"}, {"color_scheme", item.ref}});
    } else {
        ops.push_back(Json{{"op", "apply_template"}, {"template", item.ref}, {"keep_text", false}, {"language", sample.key}});
    }
    return preview(doc, ops, out, &transient, &error);
}

std::string titlelibrary::thumbnailRecipe(const LibraryItemInfo& item, const ThumbnailSample& sample) {
    titleart::TitleArtDoc doc;
    std::map<std::string, std::string> transient;
    std::string error;
    std::string material = TitleLibrary::get().itemText(item);
    if (item.kind == LibraryKind::ColorScheme) {
        // the preview look is part of what a scheme's thumbnail shows
        const std::string look = str(TitleLibrary::get().manifest(), "scheme_preview_look", "casino/outline-gold");
        if (const LibraryItemInfo* l = TitleLibrary::get().info(LibraryKind::Look, look, nullptr)) material += TitleLibrary::get().itemText(*l);
    }
    uint32_t bake = 0;
    if (thumbnailDocument(item, sample, doc, transient, error)) {
        bake = titleart::titleArtBakeVersion(doc);
        for (const auto& [asset, file] : transient) {
            std::string bytes;
            if (readBytes(file, bytes)) material += "\n" + asset + ":" + sha256Hex(bytes);
        }
    } else {
        material += "\nerror:" + error;
    }
    material += "\nbake:" + std::to_string(bake) + "\ntext:" + sample.key + ":" + sample.text + "\nsize:" + std::to_string(THUMBNAIL_WIDTH) + "x" +
                std::to_string(THUMBNAIL_HEIGHT);
    return sha256Hex(material);
}

std::string titlelibrary::readThumbnailRecipe(const fs::path& png) {
    std::string bytes;
    if (!readBytes(png, bytes) || bytes.size() < 8 || bytes.compare(1, 3, "PNG") != 0) return {};
    size_t at = 8;
    while (at + 12 <= bytes.size()) {
        const uint32_t length = ((uint32_t)(uint8_t)bytes[at] << 24) | ((uint32_t)(uint8_t)bytes[at + 1] << 16) | ((uint32_t)(uint8_t)bytes[at + 2] << 8) |
                                (uint32_t)(uint8_t)bytes[at + 3];
        const std::string type = bytes.substr(at + 4, 4);
        if (at + 12 + length > bytes.size()) break;
        if (type == "tEXt") {
            const std::string data = bytes.substr(at + 8, length);
            const size_t zero = data.find('\0');
            if (zero != std::string::npos && data.substr(0, zero) == THUMBNAIL_TEXT_KEY) return data.substr(zero + 1);
        }
        if (type == "IDAT" || type == "IEND") break;
        at += 12 + length;
    }
    return {};
}

// ---- verify ----

std::vector<VerifyFinding> titlelibrary::verifyLibrary(const VerifyOptions& options) {
    std::vector<VerifyFinding> out;
    TitleLibrary& lib = TitleLibrary::get();
    std::set<std::string> familyIds;
    for (const LibraryFamily& f : lib.families()) familyIds.insert(f.id);
    Json review = Json::array();
    if (!options.reviewFile.empty()) {
        std::string text;
        if (readBytes(options.reviewFile, text)) {
            const Json parsed = Json::parse(text, nullptr, false);
            if (parsed.is_array()) review = parsed;
            else if (parsed.is_object() && parsed.contains("records")) review = parsed["records"];
        }
    }
    std::set<std::string> reviewed;
    for (const Json& r : review) reviewed.insert(str(r, "id"));
    for (LibraryKind kind : LIBRARY_KINDS) {
        for (const LibraryItemInfo& item : lib.items(kind)) {
            if (item.scope != LibraryScope::BuiltIn) continue;
            auto add = [&](const std::string& code, const std::string& message, bool warning = false) {
                out.push_back({item.ref, code, message, warning});
            };
            const std::string text = lib.itemText(item);
            const Json raw = Json::parse(text, nullptr, false);
            if (!raw.is_object()) {
                add("invalid", "not a JSON object");
                continue;
            }
            const Json& identity = kind == LibraryKind::Template && raw.contains("template") ? raw["template"] : raw;
            // loads canonically, with no warning
            if (kind == LibraryKind::Look) {
                LookParseResult parsed = parseLook(text);
                if (!parsed.ok) add("invalid", parsed.error);
                for (const LibraryWarning& w : parsed.warnings) add("load_warning", w.toString());
                if (parsed.ok && writeLook(parsed.look) != text) add("not_canonical", "not in canonical form: run title-library normalize");
                std::vector<std::string> unknown;
                if (raw.contains("style")) unknownMembers(raw["style"], "style2d", "style", unknown);
                if (raw.contains("style_3d")) unknownMembers(raw["style_3d"], "style_3d", "style_3d", unknown);
                if (raw.contains("lighting")) unknownMembers(raw["lighting"], "lighting_3d", "lighting", unknown);
                unknownEffects(raw.value("effects", Json::array()), "effects", unknown);
                unknownEffects(raw.value("group_effects", Json::array()), "group_effects", unknown);
                if (raw.contains("part_effects")) for (auto it = raw["part_effects"].begin(); it != raw["part_effects"].end(); ++it) unknownEffects(it.value(), "part_effects." + it.key(), unknown);
                if (raw.contains("generators")) {
                    for (size_t i = 0; i < raw["generators"].size(); i++) {
                        const Json& g = raw["generators"][i];
                        if (const titleart::GeneratorDef* def = titleart::findGenerator(str(g, "type"))) {
                            unknownParams(def->params, g.value("params", Json::object()), "generators[" + std::to_string(i) + "].params", unknown);
                        }
                        unknownEffects(g.value("effects", Json::array()), "generators[" + std::to_string(i) + "].effects", unknown);
                    }
                }
                for (const std::string& u : unknown) add("unknown_member", u + " is not a member this build knows (a typo, or a later spec's)");
                if (raw.contains("font") && raw["font"].is_object()) {
                    for (const char* k : {"latin", "ja"}) {
                        const std::string id = str(raw["font"], k);
                        if (!id.empty() && !findPackFont(id) && fs::exists(TitleLibrary::packDir() / "fonts")) add("missing_font", std::string("font.") + k + " '" + id + "' is not a pack font");
                    }
                }
            } else if (kind == LibraryKind::ColorScheme) {
                titleart::ColorScheme s;
                std::vector<std::string> warnings;
                if (!titleart::parseColorScheme(raw, s, &warnings)) add("invalid", "does not parse");
                for (const std::string& w : warnings) add("load_warning", w);
                if (titleart::writeColorSchemeText(s) != text) add("not_canonical", "not in canonical form");
                if (s.lightRamp.size() < 4 || s.lightRamp.size() > 7) add("bad_light_ramp", "light_ramp holds " + std::to_string(s.lightRamp.size()) + " colours (4 to 7)");
            } else {
                TemplateParseResult parsed = parseTemplate(text);
                if (!parsed.ok) add("invalid", parsed.error);
                if (parsed.ok && writeTemplate(parsed.tmpl) != text) add("not_canonical", "not in canonical form");
                bool hasText = false;
                titleart::forEachLayer(raw, [&](const Json& l, const Json*, int) {
                    hasText = hasText || titleart::layerTypeOf(l) == titleart::LayerType::Text;
                    return !hasText;
                });
                if (hasText && identity.value("placeholders", Json::array()).empty()) add("template_without_placeholder", "a template over a Text layer needs a placeholder");
            }
            // metadata (R36)
            if (item.name.en.empty() || item.name.ja.empty()) add("missing_name", "a built-in needs name.en and name.ja");
            if (item.tags.size() < 3) add("few_tags", "a built-in needs at least 3 tags (has " + std::to_string(item.tags.size()) + ")");
            if (kind == LibraryKind::ColorScheme) {
                for (const std::string& f : item.familyHint) if (!familyIds.count(f)) add("bad_family", "family_hint '" + f + "' is not a family");
            } else if (!familyIds.count(item.family)) {
                add("bad_family", "'" + item.family + "' is not a family");
            } else if (kind != LibraryKind::ColorScheme && item.ref.rfind(item.family + "/", 0) != 0) {
                add("bad_family", "the id's family prefix is not its family (ids keep the prefix they shipped with: allowed only for a moved item)", true);
            }
            if (!identity.contains("provenance") || str(identity["provenance"], "source").empty()) add("no_provenance", "no provenance.source");
            if (!options.reviewFile.empty() && identity.contains("provenance") && str(identity["provenance"], "source") == "pachislop" && !reviewed.count(item.ref)) {
                add("no_review", "a ported item needs a review record (tools/titleart/pachislop-port/review.json)");
            }
            // denied terms in every string (R38)
            for (const DeniedTermHit& hit : findDeniedTerms(raw)) add("denied_term", hit.path + " holds the denied term '" + hit.term + "'");
            if (!item.available) add("unavailable", item.unavailableReason, true);
            // thumbnails (R32)
            if (options.thumbnails && fs::exists(TitleLibrary::packDir() / "thumbnails")) {
                for (const ThumbnailSample& s : thumbnailSamples(item)) {
                    const fs::path png = bakedThumbnailPath(kind, item.ref, s.key);
                    const std::string recipe = readThumbnailRecipe(png);
                    if (recipe.empty()) add("thumbnail_missing", png.filename().string() + " is missing (title-library bake-thumbnails)");
                    else if (recipe != thumbnailRecipe(item, s)) add("thumbnail_stale", png.filename().string() + " is stale (title-library bake-thumbnails)");
                }
            }
        }
    }
    return out;
}

// ---- docs (R47) ----

std::string titlelibrary::libraryDocsMarkdown() {
    TitleLibrary& lib = TitleLibrary::get();
    std::ostringstream md;
    const auto& looks = lib.items(LibraryKind::Look);
    const auto& schemes = lib.items(LibraryKind::ColorScheme);
    const auto& templates = lib.items(LibraryKind::Template);
    auto builtin = [](const std::vector<LibraryItemInfo>& v) {
        size_t n = 0;
        for (const auto& i : v) n += i.scope == LibraryScope::BuiltIn;
        return n;
    };
    md << "# Title Art library catalogue\n\n";
    md << "Generated by `jasperengine-editor title-library gen-docs` from the built-in library (`editor/resources/titleart`); do not edit. "
          "How to use it (the three axes, refs and scopes, the tier ladder, Save As ops): the Library section of `jasper://docs/title-art`. "
          "`list_title_library {kind, query}` returns the same items with a ready `use` op; `sheet` renders them on your own text.\n\n";
    md << builtin(looks) << " Looks, " << builtin(schemes) << " Color Schemes and " << builtin(templates) << " Templates in " << lib.families().size()
       << " families. A Look restyles one layer (`apply_look`), a Color Scheme recolours by role (`apply_color_scheme`), a Template replaces the "
          "document (`apply_template`). Items marked (unavailable) need an effect, generator or 3D type this build lacks.\n\n";
    md << "## Families\n\n";
    for (const LibraryFamily& family : lib.families()) {
        md << "### " << family.name.en << " (" << family.name.ja << ")\n\n";
        md << "Family id `" << family.id << "`.";
        std::vector<std::string> hints;
        for (const auto& s : schemes) {
            if (s.scope == LibraryScope::BuiltIn && std::find(s.familyHint.begin(), s.familyHint.end(), family.id) != s.familyHint.end()) hints.push_back("`" + s.ref + "`");
        }
        if (!hints.empty()) {
            md << " Suggested Color Schemes: ";
            for (size_t i = 0; i < hints.size(); i++) md << (i ? ", " : "") << hints[i];
            md << ".";
        }
        md << "\n\n| Look | Name | Dim | Tags |\n|---|---|---|---|\n";
        std::vector<std::string> aliases;
        for (const auto& l : looks) {
            if (l.scope != LibraryScope::BuiltIn || l.family != family.id) continue;
            md << "| `" << l.ref << "` | " << l.name.en << " / " << l.name.ja << (l.available ? "" : " (unavailable)") << " | " << l.dimension << " | ";
            for (size_t i = 0; i < l.tags.size(); i++) md << (i ? ", " : "") << l.tags[i];
            md << " |\n";
            if (!l.synonymsText.en.empty()) aliases.push_back(l.synonymsText.en);
        }
        bool any = false;
        for (const auto& t : templates) {
            if (t.scope != LibraryScope::BuiltIn || t.family != family.id) continue;
            if (!any) md << "\nTemplates: ";
            md << (any ? ", " : "") << "`" << t.ref << "` " << t.name.en << (t.available ? "" : " (unavailable)");
            any = true;
        }
        if (any) md << ".\n";
        if (!aliases.empty()) {
            std::set<std::string> words;
            for (const std::string& a : aliases) {
                std::istringstream in(a);
                std::string w;
                while (in >> w) words.insert(w);
            }
            md << "\nAlso found as: ";
            size_t i = 0;
            for (const std::string& w : words) md << (i++ ? ", " : "") << "`" << w << "`";
            md << ".\n";
        }
        md << "\n";
    }
    md << "## Color Schemes\n\n| Scheme | Name | Families | Tags |\n|---|---|---|---|\n";
    for (const auto& s : schemes) {
        if (s.scope != LibraryScope::BuiltIn) continue;
        md << "| `" << s.ref << "` | " << s.name.en << " / " << s.name.ja << " | ";
        for (size_t i = 0; i < s.familyHint.size(); i++) md << (i ? ", " : "") << s.familyHint[i];
        md << " | ";
        for (size_t i = 0; i < s.tags.size(); i++) md << (i ? ", " : "") << s.tags[i];
        md << " |\n";
    }
    return md.str();
}
