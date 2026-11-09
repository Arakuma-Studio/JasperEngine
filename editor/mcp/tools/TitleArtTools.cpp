// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The `title_art` group (S80 R53-R61, §6.11): list_title_library, get_title, edit_title,
// render_title, check_title and export_title, and the `.titleart` parts of get_asset_info and
// view_asset. edit_title and the Title Designer edit through the same TitleArtOps batches on
// TitleArtDocument (one undo step per call, attributed to the agent by McpUndo); render, check
// and bake go through TitleArtRenderHost, the one renderer. Later specs add ops, check ids,
// `options` keys and `format` values here, never tools.

#include "McpTools.h"
#include "TitleArtToolsShared.h"
#include "TitleArtToolsLibrary.h"
#include "titleart/library/TitleLibraryRegistration.h"

#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/tools/TitleArtToolsRuntime.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/fx/FxPresets.h"
#include "titleart/fx/FxRecipes.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectCatalog.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/TitlePreviewQueue.h"
#include "render/preview/TitleArtPreviewRender.h"
#include "titleart/three_d/BevelProfile.h"
#include "titleart/three_d/Title3DEditor.h"
#include "titleart/three_d/TitleGlbWriter.h"
#include "titleart/three_d/Title3DSchema.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "util/FontFace.h"
#include "util/Util.h"
#include "App.h"
#include "window/TitleDesignerWindow.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>

namespace jasperengine::editor::mcp {

namespace {

using namespace schema;
using namespace titletools;
namespace fs = std::filesystem;

std::string str(const titleart::Json& object, const char* key) {
    auto it = object.find(key);
    return (it != object.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// ==================================================================== list_title_library

// the program's `kind` enum (§6.11): S80's kinds, then each later spec's
struct LibraryKind {
    const char* kind;
    const char* label;
    const char* owner;      // "" for S80's own
};
const LibraryKind LIBRARY_KINDS[] = {
    {"effects", "Effects", ""}, {"generators", "Generators", ""}, {"blend_modes", "Blend modes", ""}, {"fonts", "Fonts", ""},
    {"looks", "Looks", "S84"}, {"color_schemes", "Color Schemes", "S84"}, {"templates", "Templates", "S84"},
    {"environments", "Environments", "S86"}, {"materials_3d", "3D materials", "S86"}, {"bevel_profiles", "Bevel profiles", "S86"},
    {"animation_presets", "Animation presets", "S85"},
};

// the variable axes (fvar) and the glyph count (maxp) of a font file's face
struct FontTables {
    std::vector<Json> axes;
    int glyphs = 0;
};

uint32_t be32(const std::string& b, size_t at) {
    if (at + 4 > b.size()) return 0;
    return (uint32_t(uint8_t(b[at])) << 24) | (uint32_t(uint8_t(b[at + 1])) << 16) | (uint32_t(uint8_t(b[at + 2])) << 8) | uint32_t(uint8_t(b[at + 3]));
}
uint16_t be16(const std::string& b, size_t at) {
    if (at + 2 > b.size()) return 0;
    return static_cast<uint16_t>((uint8_t(b[at]) << 8) | uint8_t(b[at + 1]));
}
double fixed(const std::string& b, size_t at) {
    return static_cast<int32_t>(be32(b, at)) / 65536.0;
}

FontTables readFontTables(const std::string& bytes, int faceIndex) {
    FontTables out;
    size_t offset = 0;
    if (bytes.compare(0, 4, "ttcf") == 0) {
        const uint32_t count = be32(bytes, 8);
        if (faceIndex < 0 || static_cast<uint32_t>(faceIndex) >= count) return out;
        offset = be32(bytes, 12 + 4 * static_cast<size_t>(faceIndex));
    }
    const uint16_t tables = be16(bytes, offset + 4);
    for (uint16_t i = 0; i < tables; i++) {
        const size_t record = offset + 12 + 16 * static_cast<size_t>(i);
        const std::string tag = bytes.substr(record, 4);
        const size_t at = be32(bytes, record + 8);
        if (tag == "maxp") out.glyphs = be16(bytes, at + 4);
        if (tag == "fvar") {
            const uint16_t axesOffset = be16(bytes, at + 4), axisCount = be16(bytes, at + 8), axisSize = be16(bytes, at + 10);
            for (uint16_t a = 0; a < axisCount && a < 64; a++) {
                const size_t axis = at + axesOffset + static_cast<size_t>(a) * axisSize;
                out.axes.push_back(Json{{"tag", bytes.substr(axis, 4)}, {"min", fixed(bytes, axis + 4)}, {"default", fixed(bytes, axis + 8)},
                                        {"max", fixed(bytes, axis + 12)}});
            }
        }
    }
    return out;
}

Json fontRow(const titleart::ResolvedTitleAsset& asset, const std::shared_ptr<FontFace>& face, const std::string& bytes) {
    Json row = Json::object();
    row["path"] = asset.found ? asset.assetPath : std::string("(built-in)");
    row["guid"] = asset.guid.empty() ? Json(nullptr) : Json(asset.guid);
    row["family"] = face ? face->getFamilyName() : std::string();
    if (face) {
        const int weight = face->getWeight();
        const char* names[] = {"Thin", "Extra Light", "Light", "Regular", "Medium", "Semi Bold", "Bold", "Extra Bold", "Black"};
        const int index = std::clamp((weight + 50) / 100 - 1, 0, 8);
        row["styles"] = Json::array({std::string(names[index]) + (face->isItalic() ? " Italic" : "") + " " + std::to_string(weight)});
    }
    const FontTables tables = readFontTables(bytes, face ? face->getFaceIndex() : 0);
    row["axes"] = tables.axes;
    Json scripts = Json::array();
    if (face) {
        const std::pair<const char*, uint32_t> samples[] = {{"latin", 'A'}, {"greek", 0x03a9}, {"cyrillic", 0x0416}, {"arabic", 0x0628},
                                                            {"kana", 0x3042}, {"kanji", 0x6f22}, {"hangul", 0xd55c}};
        for (const auto& [script, cp] : samples) if (face->hasCodepoint(cp)) scripts.push_back(script);
    }
    row["scripts"] = scripts;
    row["glyphs"] = tables.glyphs;
    return row;
}

bool matches(const std::string& query, const std::vector<std::string>& fields) {
    if (query.empty()) return true;
    const std::string q = lower(query);
    for (const std::string& f : fields) if (lower(f).find(q) != std::string::npos) return true;
    return false;
}

void listTitleLibraryTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    const std::string kind = args.value("kind", std::string());
    const std::string id = args.value("id", std::string());
    const std::string query = args.value("query", std::string());
    const std::string category = lower(args.value("category", std::string()));
    const int limit = args.value("limit", 20), offset = args.value("offset", 0);
    titleart::TitleEffectRegistry& registry = titleart::TitleEffectRegistry::get();
    auto inCategory = [&](const char* c) { return category.empty() || lower(c ? c : "") == category; };
    // S83 §6.11: an effect's or generator's presets (ids in rows, their params in the detail)
    auto presetsOf = [](const char* type, bool detail) {
        Json out = Json::array();
        for (const TitleFxPreset& p : TitleFxPresets::forType(type)) {
            if (!detail) {
                out.push_back(p.id);
                continue;
            }
            Json row = Json{{"id", p.id}, {"name", p.name}, {"params", fromOrdered(p.params)}};
            if (!p.cost.empty()) row["cost"] = p.cost;
            out.push_back(row);
        }
        return out;
    };
    auto liveParams = [](const titleart::LiveVariant& live) {
        Json out = Json::array();
        for (const char* k : live.params) out.push_back(k);
        return out;
    };
    auto presetFields = [](const char* type, std::vector<std::string>& fields) {
        for (const TitleFxPreset& p : TitleFxPresets::forType(type)) {
            fields.push_back(p.name);
            for (const std::string& a : p.aliases) fields.push_back(a);
        }
    };

    auto fonts = [&]() {
        std::vector<Json> rows;
        const EditorTitleAssetResolver* resolver = TitleArtRenderHost::editor().resolver(project);
        rows.push_back(fontRow({}, resolver ? resolver->builtinFont() : nullptr, std::string()));
        if (resolver) {
            for (const titleart::ResolvedTitleAsset& asset : resolver->projectFonts()) {
                std::string bytes;
                assetio::readFile(fs::path(asset.path), bytes);
                rows.push_back(fontRow(asset, resolver->loadFont(asset, 0, nullptr), bytes));
            }
        }
        return rows;
    };

    if (kind.empty()) {
        Json counts = Json::object();
        std::string text = "Title Art library:";
        for (const LibraryKind& k : LIBRARY_KINDS) {
            size_t n = 0;
            if (std::strcmp(k.kind, "effects") == 0) n = registry.effects().size();
            else if (std::strcmp(k.kind, "generators") == 0) n = registry.generators().size();
            else if (std::strcmp(k.kind, "blend_modes") == 0) n = titleart::TITLE_BLEND_COUNT;
            else if (std::strcmp(k.kind, "fonts") == 0) n = fonts().size();
            else if (std::strcmp(k.kind, "materials_3d") == 0) n = titleart::title3DMaterialPresetNames().size();
            else if (std::strcmp(k.kind, "environments") == 0) n = titleart::title3DEnvironmentNames().size();
            else if (std::strcmp(k.kind, "bevel_profiles") == 0) n = titleart::three_d::bevelProfileNames().size();
            else if (std::strcmp(k.owner, "S84") == 0) n = libraryCatalogue()[k.kind].value("total", 0);
            counts[k.kind] = n;
            text += " " + std::string(k.kind) + " " + std::to_string(n) + (*k.owner ? " (" + std::string(k.owner) + ")" : "") + ",";
        }
        text.back() = '.';
        call->ok(text, Json{{"counts", counts}, {"library", libraryCatalogue()}});
        return;
    }
    const LibraryKind* known = nullptr;
    for (const LibraryKind& k : LIBRARY_KINDS) if (kind == k.kind) known = &k;
    if (!known) {
        call->fail("Unknown kind '" + kind + "'.");
        return;
    }
    // S84's kinds: rows, details and sheets of the library (TitleArtToolsLibrary.cpp)
    if (listLibraryKind(call)) return;
    if (*known->owner && std::strcmp(known->owner, "S86") != 0) {
        call->ok(std::string(known->label) + " arrive with " + known->owner + ".",
                 Json{{"kind", kind}, {"rows", Json::array()}, {"total", 0}, {"note", std::string(known->label) + " arrive with " + known->owner}});
        return;
    }
    if (!id.empty()) {
        if (kind == "effects") {
            if (const titleart::EffectDef* def = registry.effect(id)) {
                Json detail = fromOrdered(titleart::titleEffectDetail(*def));
                detail["entry"] = "effect";
                detail["presets"] = presetsOf(def->type, true);
                detail["live_params"] = liveParams(def->live);
                Json anim = Json::object();
                for (const titleart::ParamDef& q : def->params) anim[q.key] = titleart::fxAnimName(q.anim);
                detail["anim"] = anim;
                detail["docs"] = std::string("docs/mcp/guide/title-art-effects.md#") + def->type;
                call->ok(std::string(def->label) + " (" + def->type + ")", detail);
                return;
            }
            if (const TitleFxRecipe* r = findTitleFxRecipe(id)) {
                Json op = Json{{"op", "apply_recipe"}, {"layer", "<layer>"}, {"recipe", r->id}};
                call->ok(std::string(r->name) + " (recipe: " + r->type + ")",
                         Json{{"id", r->id}, {"name", r->name}, {"entry", "recipe"}, {"category", r->category}, {"type", r->type},
                              {"preset", r->preset ? Json(r->preset) : Json(nullptr)}, {"params", fromOrdered(titleFxRecipeParams(*r))}, {"op", op}});
                return;
            }
            for (const TitleFxStyleSetting& st : titleFxStyleSettings()) {
                if (id != st.id) continue;
                Json op = Json{{"op", "set"}, {"layer", "<layer>"}, {"values", Json{{st.member, true}}}};
                call->ok(std::string(st.name) + " (style setting)", Json{{"id", st.id}, {"name", st.name}, {"entry", "style_setting"}, {"category", st.category},
                                                                       {"sets", st.member}, {"layers", st.layers}, {"op", op}});
                return;
            }
        } else if (kind == "generators") {
            if (const titleart::GeneratorDef* def = registry.generator(id)) {
                call->ok(std::string(def->label) + " (" + def->type + ")", fromOrdered(titleart::titleGeneratorDetail(*def)));
                return;
            }
        }
        std::vector<std::string> ids;
        if (kind == "effects") {
            for (const auto* d : registry.effects()) ids.push_back(d->type);
            for (const TitleFxRecipe& r : titleFxRecipes()) ids.push_back(r.id);
            for (const TitleFxStyleSetting& st : titleFxStyleSettings()) ids.push_back(st.id);
        }
        if (kind == "generators") for (const auto* d : registry.generators()) ids.push_back(d->type);
        std::string list;
        for (const std::string& i : ids) list += (list.empty() ? "" : ", ") + i;
        const std::string nearest = titleart::nearestName(id, ids);
        call->fail("No " + kind + " '" + id + "'" + (nearest.empty() ? "" : " (did you mean " + nearest + "?)") + ". Valid: " +
                   (list.empty() ? "none in this build" : list) + ".");
        return;
    }
    std::vector<Json> rows;
    if (kind == "effects") {
        for (const titleart::EffectDef* d : registry.effects()) {
            std::vector<std::string> fields = {d->type, d->label, d->category};
            for (const char* a : d->aliases) fields.push_back(a);
            presetFields(d->type, fields);
            if (!inCategory(d->category) || !matches(query, fields)) continue;
            Json row = fromOrdered(titleart::titleEffectRow(*d));
            row["entry"] = "effect";
            row["presets"] = presetsOf(d->type, false);
            row["live_params"] = liveParams(d->live);
            rows.push_back(std::move(row));
        }
        // S83 D4, D5: the menu entries that add another type's preset or switch a style setting
        for (const TitleFxRecipe& r : titleFxRecipes()) {
            std::vector<std::string> fields = {r.id, r.name, r.category, r.type};
            for (const char* a : r.aliases) fields.push_back(a);
            if (!inCategory(r.category) || !matches(query, fields)) continue;
            const titleart::EffectDef* def = registry.effect(r.type);
            rows.push_back(Json{{"id", r.id}, {"name", r.name}, {"entry", "recipe"}, {"category", r.category}, {"type", r.type},
                                {"preset", r.preset ? Json(r.preset) : Json(nullptr)},
                                {"cost", def ? titleart::costClassName(def->costClass) : "C1"}});
        }
        for (const TitleFxStyleSetting& st : titleFxStyleSettings()) {
            std::vector<std::string> fields = {st.id, st.name, st.category, st.member};
            for (const char* a : st.aliases) fields.push_back(a);
            if (!inCategory(st.category) || !matches(query, fields)) continue;
            rows.push_back(Json{{"id", st.id}, {"name", st.name}, {"entry", "style_setting"}, {"category", st.category}, {"sets", st.member},
                                {"layers", st.layers}});
        }
    } else if (kind == "generators") {
        for (const titleart::GeneratorDef* d : registry.generators()) {
            std::vector<std::string> fields = {d->type, d->label, d->category};
            for (const char* a : d->aliases) fields.push_back(a);
            presetFields(d->type, fields);
            if (!inCategory(d->category) || !matches(query, fields)) continue;
            Json row = fromOrdered(titleart::titleGeneratorRow(*d));
            row["entry"] = "generator";
            row["presets"] = presetsOf(d->type, false);
            rows.push_back(std::move(row));
        }
    } else if (kind == "blend_modes") {
        for (int i = 0; i < titleart::TITLE_BLEND_COUNT; i++) {
            const auto blend = static_cast<titleart::TitleBlend>(i);
            if (!matches(query, {titleart::titleBlendName(blend), titleart::titleBlendLabel(blend)})) continue;
            const bool fixedFunction = blend == titleart::TitleBlend::Normal || blend == titleart::TitleBlend::Add;
            rows.push_back(Json{{"id", titleart::titleBlendName(blend)}, {"name", titleart::titleBlendLabel(blend)}, {"value", i},
                                {"cost", fixedFunction ? "C0" : "C1"}});
        }
    } else if (kind == "fonts") {
        for (Json& row : fonts()) {
            if (!matches(query, {row.value("path", std::string()), row.value("family", std::string())})) continue;
            rows.push_back(std::move(row));
        }
        // the library's pack fonts (S84 R28): imported into the project by the first look or template that uses one
        for (Json& row : packFontRows(query)) rows.push_back(std::move(row));
    } else if (kind == "materials_3d") {
        // S86's material presets (R17), in the picker's sections
        for (const std::string& preset : titleart::title3DMaterialPresetNames()) {
            const titleart::TitleMaterial3D m = titleart::decodeMaterial3D(titleart::materialPreset(preset));
            const std::string section = titleart::materialPresetSection(preset);
            if (!matches(query, {preset, section})) continue;
            rows.push_back(Json{{"id", preset}, {"section", section}, {"color", titleart::formatTitleColor(m.color)}, {"metallic", m.metallic},
                                {"roughness", m.roughness}, {"clearcoat", m.clearcoat}, {"transmission", m.transmission}, {"ramp", m.gradient.has_value()}});
        }
    } else if (kind == "environments") {
        // S86's environments (R24): procedural studios, Poly Haven photos (S84's four among them), custom, matcap
        for (const std::string& env : titleart::title3DEnvironmentNames()) {
            const titleart::TitleEnvScene* scene = titleart::titleEnvironmentPreset(env);
            const char* source = scene ? (scene->procedural ? "procedural" : "photo") : env.c_str();
            if (!matches(query, {env, source})) continue;
            rows.push_back(Json{{"id", env}, {"source", source}, {"hdri", scene ? scene->hdri : std::string()},
                                {"builds", scene ? scene->procedural : env == "matcap"}});
        }
    } else if (kind == "bevel_profiles") {
        for (const std::string& profile : titleart::three_d::bevelProfileNames()) {
            if (!matches(query, {profile})) continue;
            rows.push_back(Json{{"id", profile}, {"custom", profile == "custom"}});
        }
    }
    const size_t total = rows.size();
    Json page = Json::array();
    for (size_t i = static_cast<size_t>(std::max(0, offset)); i < rows.size() && page.size() < static_cast<size_t>(std::max(1, limit)); i++) {
        page.push_back(rows[i]);
    }
    std::string text = std::to_string(total) + " " + kind;
    if (!page.empty()) text += ": " + page[0].value("id", page[0].value("path", std::string())) + (page.size() > 1 ? ", ..." : "");
    call->ok(text, Json{{"kind", kind}, {"rows", page}, {"total", total}, {"offset", offset}});
}

// ==================================================================== get_title

// the values of `layer` that differ from a new layer of its type, as {path: value}
void diffDefaults(const titleart::Json& value, const titleart::Json& defaults, const std::string& path, Json& out) {
    static const std::set<std::string> skip = {"id", "type", "children", "collapsed"};
    if (value.is_object() && defaults.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (path.empty() && skip.count(it.key())) continue;
            const std::string at = path.empty() ? it.key() : path + "." + it.key();
            auto d = defaults.find(it.key());
            if (d == defaults.end()) out[at] = fromOrdered(it.value());
            else diffDefaults(it.value(), *d, at, out);
        }
        return;
    }
    if (value != defaults) out[path] = fromOrdered(value);
}

// a field glob over a layer ("style.rims[*].width", "chars.*.rotation"): every concrete match
void matchGlob(const titleart::Json& value, const std::vector<titleart::FieldSegment>& glob, size_t at, const std::string& path, Json& out) {
    if (at == glob.size()) {
        out[path] = fromOrdered(value);
        return;
    }
    const titleart::FieldSegment& seg = glob[at];
    if (seg.isIndex || seg.key == "[*]") {
        if (!value.is_array()) return;
        if (seg.key == "[*]") {
            for (size_t i = 0; i < value.size(); i++) matchGlob(value[i], glob, at + 1, path + "[" + std::to_string(i) + "]", out);
        } else if (seg.index < value.size()) {
            matchGlob(value[seg.index], glob, at + 1, path + "[" + std::to_string(seg.index) + "]", out);
        }
        return;
    }
    if (!value.is_object()) return;
    if (seg.key == "*") {
        for (auto it = value.begin(); it != value.end(); ++it) matchGlob(it.value(), glob, at + 1, path.empty() ? it.key() : path + "." + it.key(), out);
        return;
    }
    auto it = value.find(seg.key);
    if (it != value.end()) matchGlob(*it, glob, at + 1, path.empty() ? seg.key : path + "." + seg.key, out);
}

bool parseGlob(const std::string& text, std::vector<titleart::FieldSegment>& out) {
    // [*] is not a field path index: mark it as a wildcard segment
    std::string replaced;
    std::vector<size_t> wildcardPositions;
    for (size_t i = 0; i < text.size(); i++) {
        if (text.compare(i, 3, "[*]") == 0) {
            replaced += "[0]";
            wildcardPositions.push_back(replaced.size());
            i += 2;
        } else {
            replaced += text[i];
        }
    }
    if (!titleart::parseFieldPath(replaced, out, nullptr)) return false;
    // the wildcard indexes, in order
    size_t w = 0;
    size_t position = 0;
    for (titleart::FieldSegment& seg : out) {
        if (seg.isIndex) {
            position = replaced.find("[0]", position);
            if (w < wildcardPositions.size() && position != std::string::npos && position + 3 == wildcardPositions[w]) {
                seg.isIndex = false;
                seg.key = "[*]";
                w++;
            }
            if (position != std::string::npos) position += 3;
        }
    }
    return true;
}

Json layerSummary(const titleart::TitleArtDoc& doc, const titleart::Json& layer, TitleOpContext& names, const std::string& detail,
                  const std::vector<std::string>& fields) {
    Json row = Json::object();
    const std::string id = str(layer, "id");
    const titleart::LayerType type = titleart::layerTypeOf(layer);
    row["path"] = names.layerPath(id);
    row["id"] = id;
    row["type"] = str(layer, "type");
    row["name"] = str(layer, "name");
    if (type == titleart::LayerType::Text) {
        row["text"] = str(layer, "text");
        const titleart::TitleAssetRef font = titleart::decodeAssetRef(layer.contains("font") ? layer["font"] : titleart::Json());
        row["font"] = font.empty() ? Json("(built-in)") : Json(font.path.empty() ? font.guid : font.path);
    }
    if (layer.contains("look") && !str(layer["look"], "id").empty()) row["look"] = fromOrdered(layer["look"]);
    if (titleart::layerTypeHasStyle(type) && layer.contains("style")) {
        Json parts = Json::array({"face"});
        if (layer["style"].contains("rims")) {
            for (size_t i = 0; i < layer["style"]["rims"].size(); i++) parts.push_back("rim#" + std::to_string(i));
        }
        row["parts"] = parts;
    }
    Json effects = Json::array();
    if (layer.contains("effects")) {
        for (const auto& e : layer["effects"]) effects.push_back("fx:" + str(e, "type") + (e.value("enabled", true) ? "" : " (off)"));
    }
    row["effects"] = effects;
    Json masks = Json::array();
    if (layer.contains("masks")) for (const auto& m : layer["masks"]) masks.push_back(str(m, "type"));
    row["masks"] = masks;
    row["clip"] = layer.value("clip", false);
    row["blend"] = str(layer, "blend");
    if (!layer.value("visible", true)) row["visible"] = false;
    if (detail == "overrides" || detail == "full") {
        const titleart::Json defaults = titleart::TitleArtSchema::get().defaultObject("layer", str(layer, "type"));
        Json overrides = Json::object();
        diffDefaults(layer, defaults, "", overrides);
        row["overrides"] = overrides;
    }
    if (detail == "full") {
        titleart::Json copy = layer;
        copy.erase("children");
        row["layer"] = fromOrdered(copy);
    }
    if (!fields.empty()) {
        Json values = Json::object();
        for (const std::string& f : fields) {
            std::vector<titleart::FieldSegment> glob;
            const titleart::ExpandedFieldPath expanded = titleart::expandFieldShorthand(&layer, f);
            const std::string path = expanded.ok ? expanded.path : f;
            if (parseGlob(path, glob)) matchGlob(layer, glob, 0, "", values);
        }
        row["fields"] = values;
    }
    (void)doc;
    return row;
}

Json problemsOf(Project* project, const titleart::TitleArtDoc& doc, const std::vector<titleart::TitleWarning>& warnings) {
    Json list = Json::array();
    for (const titleart::TitleProblem& p : titleart::titleArtDocumentProblems(doc)) list.push_back(fromOrdered(p.toJson()));
    if (const EditorTitleAssetResolver* resolver = TitleArtRenderHost::editor().resolver(project)) {
        for (const titleart::TitleProblem& p : titleArtMissingAssets(doc, *resolver)) list.push_back(fromOrdered(p.toJson()));
        for (const titleart::TitleProblem& p : titleArtMissingGlyphs(doc, *resolver)) list.push_back(fromOrdered(p.toJson()));
    }
    for (const titleart::TitleWarning& w : warnings) {
        list.push_back(Json{{"severity", "warning"}, {"code", "normalized"}, {"path", w.path}, {"message", w.toString()}});
    }
    return list;
}

Json outputsOf(Project* project, const fs::path& file, const titleart::TitleArtDoc& doc) {
    Json list = Json::array();
    const TitleBakeStatus status = titleArtStatusOf(project, file, doc);
    for (const TitleOutputState& o : status.outputs) list.push_back(fromOrdered(o.toJson(project->getProjectPath())));
    return list;
}

// the editor's preview queue (R45): every title preview of the editor shares its per-frame budget
Json previewQueueJson() {
    const TitlePreviewQueue& queue = TitlePreviewQueue::editor();
    const TitlePreviewStats& stats = queue.stats();
    const titleart::TitleStepCosts& costs = titleArtPreviewCosts();
    const auto ms = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    Json overruns = Json::array();
    for (const TitlePreviewOverrun& o : stats.overruns) {
        overruns.push_back(Json{{"frame", o.frame}, {"ms", ms(o.ms)}, {"side_work_ms", ms(o.sideWorkMs)},
                                {"longest", Json{{"label", o.longestLabel}, {"ms", ms(o.longestMs)}}}});
    }
    return Json{{"budget_ms", ms(stats.budgetMs)},
                {"queued", stats.queued},
                {"working_frames", stats.workingFrames},
                {"over_budget_frames", stats.overBudgetFrames},
                {"alone_frames", stats.aloneFrames},
                {"p95_ms", ms(queue.p95FrameMs())},
                {"p95_frames", queue.windowFrames()},
                {"worst_frame_ms", ms(stats.maxFrameMs)},
                {"worst_step", Json{{"label", stats.worstStepLabel}, {"ms", ms(stats.worstStepMs)}}},
                {"side_work_ms", ms(stats.sideWorkMs)},
                {"late_starts", stats.lateStarts},
                {"prediction_error", Json{{"first", ms(costs.firstError())}, {"recent", ms(costs.recentError())},
                                          {"observations", costs.observations()}}},
                {"recent_overruns", overruns}};
}

void getTitleTool(const CallPtr& call) {
    // S81 R69: get_title {entity} reads a Title Art Renderer's state
    if (getTitleInstance(call)) return;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    fs::path file;
    std::string relative, error;
    if (!resolveTitlePath(project, args.value("path", std::string()), true, file, relative, error)) {
        call->fail(error);
        return;
    }
    std::shared_ptr<TitleArtDocument> document = openTitle(project, file, error);
    if (!document) {
        call->fail(error);
        return;
    }
    Json out = Json::object();
    out["path"] = relative;
    out["guid"] = document->guid();
    out["rev"] = document->revision();
    out["version"] = document->versionRead();
    if (!document->editable()) {
        out["version_newer"] = document->versionError();
        call->ok(relative + ": " + document->versionError() + " (read-only here)", out);
        return;
    }
    const titleart::TitleArtDoc& doc = document->doc();
    out["canvas"] = fromOrdered(doc.root["canvas"]);
    const std::string detail = args.value("detail", std::string("outline"));
    std::vector<std::string> fields;
    if (args.contains("fields")) for (const Json& f : args["fields"]) if (f.is_string()) fields.push_back(f.get<std::string>());
    titleart::TitleArtDoc copy = doc;
    titleart::TitleIdGenerator ids;
    TitleOpsResult unused;
    TitleOpContext names(copy, ids, unused);
    Json layers = Json::array();
    std::string only;
    if (args.contains("layer")) {
        titleart::Json* layer = names.resolveLayer(args.value("layer", std::string()), "layer");
        if (!layer) {
            const TitleOpProblem& p = unused.problems.empty() ? TitleOpProblem() : unused.problems.front();
            std::string valid;
            for (const std::string& v : p.validHere) valid += (valid.empty() ? "" : ", ") + v;
            call->fail("layer: " + p.message + (valid.empty() ? "" : " Layers: " + valid + "."));
            return;
        }
        only = str(*layer, "id");
    }
    titleart::forEachLayer(doc.root, [&](const titleart::Json& layer, const titleart::Json*, int) {
        if (!only.empty() && str(layer, "id") != only) return true;
        Json summary = layerSummary(doc, layer, names, detail, fields);
        // S86: a 3D layer's effective style and lighting, each lighting value with its source
        if (titleart::layerTypeHasStyle(titleart::layerTypeOf(layer)) && str(layer, "dimension") == "3d") {
            summary["three_d"] = fromOrdered(title3DLayerInfo(layer, doc));
        }
        layers.push_back(std::move(summary));
        return true;
    });
    out["layers"] = layers;
    const titleart::Json stats3d = title3DStats(doc, TitleArtRenderHost::editor().resolver(project), titleart::TitleTier::Bake);
    if (!stats3d.empty()) out["stats_3d"] = fromOrdered(stats3d);
    out["exposed"] = fromOrdered(doc.root["exposed"]);
    out["outputs"] = outputsOf(project, file, doc);
    out["problems"] = problemsOf(project, doc, document->warnings());
    if (document->migrated()) out["migrated_from"] = document->versionRead();
    out["previews"] = previewQueueJson();
    // the Title Designer's view of this document while it shows it (S82 R80): no schema change
    if (TitleDesignerWindow* designer = call->ctx().app().getTitleDesignerWindow();
        designer && designer->isOpen() && designer->documentPath().lexically_normal() == file.lexically_normal()) {
        const Json state = fromOrdered(designer->state());
        out["designer"] = {{"selection", state.value("selection", Json::array())},
                           {"staged", state.value("staged", false)},
                           {"preview_active", state.value("preview_active", false)},
                           {"view", {{"zoom", state.value("zoom", Json(1))}, {"background", state.value("background", Json("checker"))},
                                     {"device", state.value("device", Json())}}}};
    }
    size_t stale = 0;
    for (const Json& o : out["outputs"]) if (o.value("status", std::string()) != "fresh") stale++;
    call->ok(relative + " rev " + std::to_string(document->revision()) + ": " + std::to_string(titleart::countLayers(doc.root)) + " layers, canvas " +
                 std::to_string(doc.canvasWidth()) + " x " + std::to_string(doc.canvasHeight()) + ", " + std::to_string(out["outputs"].size()) +
                 " output(s), " + std::to_string(stale) + " not fresh, " + std::to_string(out["problems"].size()) + " problem(s).",
             out);
}

// ==================================================================== edit_title

Json opsResultJson(const TitleOpsResult& result) {
    Json out = fromOrdered(result.toJson());
    return out;
}

void editTitleTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    // S84 R44: a batch of library ops alone needs no document
    if (editLibraryOnly(call)) return;
    const std::string path = args.value("path", std::string());
    fs::path file;
    std::string relative, error;
    if (!resolveTitlePath(project, path, false, file, relative, error)) {
        call->fail(error + " Nothing was applied.");
        return;
    }
    std::error_code ec;
    Json structured = Json::object();
    std::string text;
    std::shared_ptr<TitleArtDocument> document;
    if (args.contains("create")) {
        const Json& create = args["create"];
        titleart::TitleArtDoc initial;
        std::vector<TitleOpFile> createFiles;
        std::string createName;
        if (create.contains("template") || create.contains("look")) {
            // S84: the template's document (or a text in a look), its fonts imported in the same step
            if (!createFromLibrary(project, create, initial, createFiles, createName, error)) {
                call->fail(error + " Nothing was applied.");
                return;
            }
        } else if (create.contains("from")) {
            std::string code;
            if (!TitleArtOps::createFrom(create.value("from", std::string()), initial, code, error)) {
                call->fail(error + " Nothing was applied.", Json{{"code", code}});
                return;
            }
        } else {
            titleart::NewTitleArtOptions options;
            if (create.contains("text") && create["text"].is_string()) options.text = create["text"].get<std::string>();
            initial = titleart::makeNewTitleArt(options);
        }
        if (create.contains("canvas")) {
            const Json& canvas = create["canvas"];
            if (!canvas.is_array() || canvas.size() != 2 || !canvas[0].is_number() || !canvas[1].is_number()) {
                call->fail("create.canvas is [width, height] in pixels. Nothing was applied.");
                return;
            }
            initial.root["canvas"]["width"] = canvas[0].get<int>();
            initial.root["canvas"]["height"] = canvas[1].get<int>();
        }
        const bool exists = fs::exists(file, ec);
        if (exists && !create.value("overwrite", false)) {
            call->fail("'" + relative + "' exists already: pass create.overwrite true to replace it (one undo step), or edit it with ops. Nothing was applied.");
            return;
        }
        if (exists) {
            document = openTitle(project, file, error);
            if (!document) {
                call->fail(error + " Nothing was applied.");
                return;
            }
            if (!document->commit(initial, "Replace Title Art " + file.filename().string(), {}, &error, TitleArtChangeOrigin::Agent)) {
                call->fail(error + " Nothing was applied.");
                return;
            }
        } else {
            std::string escape = McpRefs::writeEscape(project, file);
            if (!escape.empty()) {
                call->fail(escape + " Nothing was applied.");
                return;
            }
            document = TitleArtDocument::create(project, file.string(), &initial, createName.empty() ? "Create Title Art " + file.filename().string() : createName,
                                                &error, &createFiles);
            if (!document) {
                call->fail(error + " Nothing was applied.");
                return;
            }
        }
        structured["created"] = true;
        text = (exists ? "Replaced " : "Created ") + relative;
    } else {
        if (!fs::is_regular_file(file, ec)) {
            call->fail("'" + relative + "' does not exist: create it with {create: {}} (and ops in the same call). Nothing was applied.");
            return;
        }
        document = openTitle(project, file, error);
        if (!document) {
            call->fail(error + " Nothing was applied.");
            return;
        }
    }

    const bool dryRun = args.value("dry_run", false);
    if (args.contains("ops")) {
        TitleArtEditOptions options;
        options.expectRev = args.value("expect_rev", static_cast<int64_t>(-1));
        options.dryRun = dryRun;
        options.origin = TitleArtChangeOrigin::Agent;
        const TitleOpsResult result = document->edit(toOrdered(args["ops"]), options);
        Json outcome = opsResultJson(result);
        for (auto it = outcome.begin(); it != outcome.end(); ++it) structured[it.key()] = it.value();
        if (!result.ok) {
            structured["path"] = relative;
            structured["rev"] = document->revision();
            call->fail(result.error(), structured);
            return;
        }
        text += (text.empty() ? "" : "; ") + std::string(dryRun ? "Would apply " : "Applied ") + std::to_string(result.applied) + " op(s) to " +
                relative + (result.undoName.empty() ? "" : " (" + result.undoName + ")");
    } else if (!args.contains("create")) {
        call->fail("Give `ops` (a list of op objects) or `create`. Nothing was applied.");
        return;
    }
    structured["path"] = relative;
    structured["rev"] = document->revision();

    // bake: auto (Auto Bake decides), now (bakes before replying), never (drops a scheduled one)
    const std::string bake = args.value("bake", std::string("auto"));
    Json bakeJson = Json{{"status", dryRun ? "skipped" : bake == "never" ? "never" : "auto"}};
    if (!dryRun && bake == "now") {
        const TitleBakeResult baked = bakeTitleArt(project, file, document->doc(), {});
        bakeJson = fromOrdered(baked.toJson());
        bakeJson["status"] = baked.ok ? "baked" : "failed";
    } else if (!dryRun && (bake == "auto" && args.contains("create"))) {
        // a new document bakes at once, so a texture exists to place (R7)
        const TitleBakeResult baked = bakeTitleArt(project, file, document->doc(), {});
        bakeJson = fromOrdered(baked.toJson());
        bakeJson["status"] = baked.ok ? "baked" : "failed";
    }
    structured["bake"] = bakeJson;
    if (text.empty()) text = "Nothing changed in " + relative;

    if (args.contains("render") && !dryRun && !(args["render"].is_boolean() && !args["render"].get<bool>())) {
        RenderSpec spec;
        spec.file = file;
        spec.relative = relative;
        spec.doc = std::make_shared<titleart::TitleArtDoc>(document->doc());
        spec.rev = document->revision();
        spec.args = args["render"].is_object() ? args["render"] : Json::object();
        static const std::set<std::string> renderKeys = {"max_width", "backgrounds", "crop"};
        for (auto it = spec.args.begin(); it != spec.args.end(); ++it) {
            if (!renderKeys.count(it.key())) {
                structured["render_error"] = "render takes true or {max_width, backgrounds, crop}; '" + it.key() + "' is not one of them";
                call->ok(text + ".", structured);
                return;
            }
        }
        startRender(call, std::move(spec), structured, text + ". ");
        return;
    }
    call->ok(text + ".", structured);
}

// ==================================================================== export_title

struct FormatOwner {
    const char* format;
    const char* owner;
};
const FormatOwner FORMATS[] = {{"bake", "S80"}, {"glb", "S86"}, {"png", "S87"}, {"psd", "S87"}, {"svg", "S87"}, {"sheet", "S87"},
                               {"atlas", "S87"}, {"sequence", "S87"}, {"ktx2", "S87"}, {"csv", "S87"}, {"xliff", "S87"}};

// the GLB of a title's 3D layers (S86, below)
void exportTitleGlb(const CallPtr& call, Project* project, const fs::path& file, const std::string& relative);

void exportTitleTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    fs::path file;
    std::string relative, error;
    if (!resolveTitlePath(project, args.value("path", std::string()), true, file, relative, error)) {
        call->fail(error);
        return;
    }
    const std::string format = args.value("format", std::string("bake"));
    if (format == "glb") {
        exportTitleGlb(call, project, file, relative);
        return;
    }
    for (const FormatOwner& f : FORMATS) {
        if (format == f.format && std::strcmp(f.owner, "S80") != 0 && std::strcmp(f.owner, "S86") != 0) {
            call->fail("not_available: format '" + format + "' arrives with " + f.owner + "; this build exports format bake.",
                       Json{{"code", "not_available"}, {"owner", f.owner}, {"formats", Json::array({"bake"})}});
            return;
        }
    }
    TitleBakeOptions options;
    if (args.contains("options")) {
        const Json& o = args["options"];
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() == "bleed_px" && it->is_number_integer() && it->get<int>() >= 0 && it->get<int>() <= 64) {
                options.bleedPx = it->get<int>();
                continue;
            }
            call->fail("bad_option: format bake takes options {bleed_px: 0..64}; '" + it.key() + "' is not valid. Nothing was written.",
                       Json{{"code", "bad_option"}, {"valid", Json::array({"bleed_px"})}});
            return;
        }
    }
    if (args.contains("outputs")) for (const Json& o : args["outputs"]) if (o.is_string()) options.outputs.push_back(o.get<std::string>());
    options.force = args.value("force", false);
    options.dryRun = args.value("dry_run", false);
    titleart::TitleArtDoc doc;
    std::string code;
    if (!loadTitleArtForBake(project, file, doc, code, error)) {
        call->fail(code + ": " + error + ". Nothing was written.", Json{{"code", code}});
        return;
    }
    auto job = std::make_shared<TitleArtBakeJob>(project, file, std::move(doc), options);
    auto task = std::make_shared<FrameTask>();
    std::weak_ptr<FrameTask> weakTask = task;
    task->tick = [job, relative, project, weakTask](McpJob& mcpJob) {
        auto t = weakTask.lock();
        if (!t) return true;
        const bool done = job->step(8.0);
        mcpJob.setProgress(job->progress(), job->progressText());
        if (!done) return false;
        const TitleBakeResult& result = job->result();
        Json out = fromOrdered(result.toJson());
        out["path"] = relative;
        if (!result.ok) {
            mcpJob.fail(result.error, out);
            return true;
        }
        size_t written = 0;
        for (const TitleBakeFile& f : result.files) written += f.status == "written" ? 1 : 0;
        t->text = (written ? "Baked " + std::to_string(written) + " output(s) of " : std::string("Nothing to write for ")) + relative;
        if (!result.files.empty()) t->text += ": " + result.files.front().path + " " + std::to_string(result.files.front().width) + " x " +
                                              std::to_string(result.files.front().height) + " (" + result.files.front().status + ")";
        if (!result.warnings.empty()) t->text += "; " + std::to_string(result.warnings.size()) + " warning(s)";
        t->text += ".";
        (void)project;
        mcpJob.succeed(t->text, out);
        return true;
    };
    runFrameTask(call, "export_title", "Bake " + relative, task);
}

} // namespace

// ==================================================================== get_asset_info / view_asset

bool titleArtAssetInfo(Project* project, const fs::path& file, Json& data, std::string& text) {
    std::string error;
    std::shared_ptr<TitleArtDocument> document = openTitle(project, file, error);
    if (!document) {
        data["title_art"] = Json{{"error", error}};
        text += ". " + error;
        return false;
    }
    Json info = Json::object();
    info["rev"] = document->revision();
    if (!document->editable()) {
        info["version_newer"] = document->versionError();
        data["title_art"] = info;
        text += ". " + document->versionError();
        return true;
    }
    const titleart::TitleArtDoc& doc = document->doc();
    info["canvas"] = Json::array({doc.canvasWidth(), doc.canvasHeight()});
    Json counts = Json::object();
    titleart::forEachLayer(doc.root, [&](const titleart::Json& layer, const titleart::Json*, int) {
        const std::string type = str(layer, "type");
        counts[type] = counts.value(type, 0) + 1;
        return true;
    });
    info["layers"] = counts;
    Json fonts = Json::array();
    std::set<std::string> seen;
    const EditorTitleAssetResolver* resolver = TitleArtRenderHost::editor().resolver(project);
    titleart::forEachLayer(doc.root, [&](const titleart::Json& layer, const titleart::Json*, int) {
        if (titleart::layerTypeOf(layer) != titleart::LayerType::Text) return true;
        std::vector<titleart::TitleAssetRef> refs = {titleart::decodeAssetRef(layer.contains("font") ? layer["font"] : titleart::Json())};
        if (layer.contains("fallback_fonts")) for (const auto& f : layer["fallback_fonts"]) refs.push_back(titleart::decodeAssetRef(f));
        for (const titleart::TitleAssetRef& ref : refs) {
            const std::string name = ref.empty() ? "(built-in)" : (ref.path.empty() ? ref.guid : ref.path);
            if (!seen.insert(name).second) continue;
            const bool missing = !ref.empty() && (!resolver || !resolver->resolve(ref, titleart::TitleAssetKind::Font).found);
            fonts.push_back(Json{{"font", name}, {"missing", missing}});
        }
        return true;
    });
    info["fonts"] = fonts;
    info["outputs"] = outputsOf(project, file, doc);
    info["problems"] = problemsOf(project, doc, document->warnings());
    size_t fresh = 0;
    for (const Json& o : info["outputs"]) fresh += o.value("status", std::string()) == "fresh" ? 1 : 0;
    data["title_art"] = info;
    text += ". Title Art " + std::to_string(doc.canvasWidth()) + " x " + std::to_string(doc.canvasHeight()) + ", " +
            std::to_string(titleart::countLayers(doc.root)) + " layers, outputs " + std::to_string(fresh) + "/" +
            std::to_string(info["outputs"].size()) + " fresh, " + std::to_string(info["problems"].size()) + " problem(s)";
    return true;
}

Json titleArtBakedPngInfo(Project* project, const fs::path& file) {
    std::string error;
    const std::optional<TitleBakeRecipe> recipe = readTitleBakeRecipe(file, &error);
    if (!recipe) return nullptr;
    Json info = Json::object();
    info["source"] = recipe->sourcePath;
    info["guid"] = recipe->sourceGuid;
    info["output"] = recipe->output;
    info["frame"] = Json::array({recipe->frame.x, recipe->frame.y, recipe->frame.width, recipe->frame.height});
    info["scale"] = recipe->scale;
    info["bit_depth"] = recipe->bitDepth;
    const fs::path document = project ? project->getAssetDatabase().getPath(recipe->sourceGuid) : fs::path();
    if (document.empty()) {
        info["status"] = "source_missing";
        return info;
    }
    const fs::path absolute = project->getProjectPath() / document;
    titleart::TitleArtDoc doc;
    std::string code;
    if (!loadTitleArtForBake(project, absolute, doc, code, error)) {
        info["status"] = code;
        return info;
    }
    info["source"] = project->normalizeToProjectRelative(absolute).generic_string();
    info["status"] = "missing";
    for (const TitleOutputState& o : titleArtStatusOf(project, absolute, doc).outputs) {
        if (o.id != recipe->output) continue;
        info["status"] = titleOutputStatusName(o.status);
        if (!o.changed.empty()) info["changed"] = o.changed;
    }
    info["regenerate"] = Json{{"tool", "export_title"}, {"arguments", Json{{"path", info["source"]}, {"outputs", Json::array({recipe->output})}}}};
    return info;
}

bool titleArtViewImage(Project* project, const fs::path& file, int maxSize, texsynth::Image& out, std::string& label, std::string& error) {
    std::shared_ptr<TitleArtDocument> document = openTitle(project, file, error);
    if (!document) return false;
    if (!document->editable()) {
        error = document->versionError();
        return false;
    }
    const titleart::TitleArtDoc& doc = document->doc();
    // the fresh main output as baked
    const TitleBakeStatus status = titleArtStatusOf(project, file, doc);
    for (const TitleOutputState& o : status.outputs) {
        if (o.status != TitleOutputStatus::Fresh) continue;
        texsynth::Image png;
        if (texsynth::loadImage(o.file, png, error)) {
            out = downscaleTo(texsynth::toRgba8(png), maxSize > 0 ? maxSize : 512);
            label = "fresh output '" + o.id + "' (" + o.fileName + ")";
            return true;
        }
    }
    // else a 512 px render
    titleart::RenderRequest request;
    request.doc = std::make_shared<titleart::TitleArtDoc>(doc);
    const int side = std::max(doc.canvasWidth(), doc.canvasHeight());
    request.pxScale = std::min(1.0f, static_cast<float>(maxSize > 0 ? maxSize : 512) / static_cast<float>(std::max(1, side)));
    TitleRenderedImage image;
    if (!TitleArtRenderHost::editor().render(project, request, image, error)) return false;
    out = image.image;
    label = "a render (no fresh output)";
    return true;
}

// ==================================================================== export_title format glb (S86)

namespace {

void exportTitleGlb(const CallPtr& call, Project* project, const fs::path& file, const std::string& relative) {
    const Json& args = call->args();
    TitleGlbOptions options;
    if (args.contains("options")) {
        const Json& o = args["options"];
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (it.key() != "glb" || !it->is_object()) {
                call->fail("bad_option: format glb takes options {glb: {units_per_px, include_bands}}; '" + it.key() + "' is not valid. Nothing was written.",
                           Json{{"code", "bad_option"}, {"valid", Json::array({"glb"})}});
                return;
            }
            for (auto g = it->begin(); g != it->end(); ++g) {
                if (g.key() == "units_per_px" && g->is_number() && g->get<double>() > 0) options.unitsPerPx = g->get<double>();
                else if (g.key() == "include_bands" && g->is_boolean()) options.includeBands = g->get<bool>();
                else {
                    call->fail("bad_option: options.glb takes units_per_px (> 0) and include_bands; '" + g.key() + "' is not valid. Nothing was written.",
                               Json{{"code", "bad_option"}, {"valid", Json::array({"units_per_px", "include_bands"})}});
                    return;
                }
            }
        }
    }
    titleart::TitleArtDoc doc;
    std::string code, error;
    if (!loadTitleArtForBake(project, file, doc, code, error)) {
        call->fail(code + ": " + error + ". Nothing was written.", Json{{"code", code}});
        return;
    }
    const TitleGlbResult glb = writeTitleGlb(doc, TitleArtRenderHost::editor().resolver(project), options);
    if (!glb.ok) {
        call->fail(glb.error + ". Nothing was written.", Json{{"code", glb.error.substr(0, glb.error.find(':'))}});
        return;
    }
    fs::path out = file;
    out.replace_extension(".glb");
    if (!args.value("dry_run", false) &&
        !assetio::writeFile(out, std::string_view(reinterpret_cast<const char*>(glb.bytes.data()), glb.bytes.size()), assetio::Durability::Document, &error)) {
        call->fail("write_failed: " + error + ". Nothing was written.", Json{{"code", "write_failed"}});
        return;
    }
    std::string written = relative;
    const size_t dot = written.rfind('.');
    if (dot != std::string::npos) written = written.substr(0, dot);
    written += ".glb";
    Json warnings = Json::array();
    for (const std::string& w : glb.warnings) warnings.push_back(w);
    call->ok(written + ": " + std::to_string(glb.layers) + " 3D layer(s), " + std::to_string(glb.triangles) + " triangles" +
                 (glb.warnings.empty() ? "." : ", " + std::to_string(glb.warnings.size()) + " warning(s)."),
             Json{{"format", "glb"}, {"path", written}, {"bytes", glb.bytes.size()}, {"layers", glb.layers}, {"triangles", glb.triangles},
                  {"warnings", warnings}, {"dry_run", args.value("dry_run", false)}});
}

} // namespace

// ==================================================================== registration

void registerTitleArtTools(McpRegistry& registry) {
    registerBuiltinTitleChecks();
    registerTitleFxOps();
    registerTitle3DEditor();
    titlelibrary::registerTitleLibrary();
    {
        ToolDef tool;
        tool.name = "list_title_library";
        tool.title = "List Title Library";
        tool.description =
            "The Title Art catalogue. No args: counts per kind. kind: rows (looks, color_schemes, templates carry a ready `use` op). "
            "id: one item. query: names en/ja, aliases, tags; category, family, dimension, tags, scope, favorites filter. "
            "sheet: items rendered on your text or layer, numbered: pick by seeing.";
        std::vector<std::string> kinds;
        for (const LibraryKind& k : LIBRARY_KINDS) kinds.push_back(k.kind);
        tool.inputSchema = object({
            {"kind", stringEnum("", kinds)},
            {"id", string("")},
            {"query", string("")},
            {"category", string("")},
            {"family", string("")},
            {"dimension", stringEnum("", {"2d", "3d"})},
            {"tags", array(string(""), "")},
            {"scope", withDefault(stringEnum("", {"builtin", "project", "user", "all"}), "all")},
            {"favorites", boolean("")},
            {"sheet", object({
                {"text", string("")},
                {"ids", array(string(""), "")},
                {"path", string("")},
                {"layer", string("")},
                {"cell", array(integer(""), "")},
                {"columns", integer("", 1, 5)},
                {"background", stringEnum("", {"black", "white", "checker", "machine"})},
                {"color_scheme", string("")},
            })},
            {"limit", withDefault(integer("", 1, 200), 20)},
            {"offset", integer("", 0, 100000)},
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.group = "title_art";
        tool.handler = listTitleLibraryTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_title";
        tool.title = "Get Title";
        tool.description =
            "Reads a .titleart: rev, canvas, layers (path, id, type, text, font, parts, effects as fx:<type>, masks, clip, blend), "
            "exposed, outputs (fresh/stale/missing), problems, previews (the editor's preview budget: frames over it, worst step). "
            "detail overrides: non-default values; full: layer objects. fields: "
            "exact values by glob (style.rims[*].width). Document y is up. entity instead of path: a Title Art Renderer's state "
            "(ready, pending, overrides, effective, live, particles); wait_ready waits for its image.";
        tool.inputSchema = object({
            {"path", string("")},
            {"entity", entityRef()},
            {"scene", sceneRef()},
            {"wait_ready", boolean("")},
            {"timeout_seconds", number("")},
            {"layer", string("")},
            {"detail", withDefault(stringEnum("", {"outline", "overrides", "full"}), "outline")},
            {"fields", array(string(""), "")},
        });
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.activity = ToolActivity::Read;
        tool.group = "title_art";
        tool.handler = getTitleTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "edit_title";
        tool.title = "Edit Title";
        tool.description =
            "Edits a .titleart with ops, all or nothing, one undo step. Ops: set {layer?, values: {path: value}}, set_text, "
            "add_layer/remove_layer/duplicate_layer/move_layer/group/ungroup, add_effect/remove_effect/move_effect, "
            "add_rim/remove_rim/move_rim, add_mask/remove_mask, expose/unexpose, link_material/unlink_material; later specs add "
            "recipe, library, animation, 3D, language and import ops (jasper://docs/title-art#ops). Layers by name path or "
            "id:<hex>; fields by dot path, face., rim#n., fx:<type>. create makes the file. Errors list valid names.";
        Json render = Json{{"type", Json::array({"boolean", "object"})}};
        tool.inputSchema = object({
            {"path", string("")},
            {"create", object({
                {"canvas", array(integer(""), "")},
                {"text", string("")},
                {"overwrite", boolean("")},
                {"template", string("")},
                {"look", string("")},
                {"language", stringEnum("", {"en", "ja"})},
                {"from", string("")},
            })},
            {"ops", array(anyObject(""), "")},
            {"dry_run", boolean("")},
            {"expect_rev", integer("")},
            {"render", render},
            {"bake", withDefault(stringEnum("", {"auto", "now", "never"}), "auto")},
        });
        tool.annotations.idempotent = false;
        tool.writesFiles = [](const Json& args) { return !args.value("dry_run", false); };
        tool.activity = ToolActivity::Edit;
        tool.group = "title_art";
        tool.handler = editTitleTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "render_title";
        tool.title = "Render Title";
        tool.description =
            "Renders a .titleart with real alpha on backgrounds (checker, black, white, #rrggbb, transparent, an image) plus "
            "alpha_bounds, coverage_pct, flat. ops / try (what-if sheets) leave the file alone. compare: rev:<n> or a PNG. "
            "layer isolates; output: its frame and content; crop: canvas px. entity: an instance's state. More: jasper://docs/title-art.";
        tool.inputSchema = object({
            {"path", string("")},
            {"entity", entityRef()},
            {"scene", sceneRef()},
            {"ops", array(anyObject(""), "")},
            {"try", array(object({{"label", string("")}, {"ops", array(anyObject(""), "")}}), "")},
            {"layer", string("")},
            {"output", string("")},
            {"backgrounds", array(string(""), "")},
            {"crop", array(integer(""), "")},
            {"max_width", withDefault(integer(""), 768)},
            {"columns", integer("")},
            {"compare", string("")},
            {"image", withDefault(stringEnum("", {"always", "none"}), "always")},
            {"fallback", stringEnum("", {"rgba8", "compare"})},
            {"options", anyObject("")},
            {"save_path", string("")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number(""), 120)},
        });
        tool.inputSchema["properties"]["try"]["maxItems"] = 9;
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.writesFiles = [](const Json& args) { return args.contains("save_path"); };
        tool.activity = ToolActivity::Read;
        tool.group = "title_art";
        tool.handler = titletools::renderTitleTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "check_title";
        tool.title = "Check Title";
        tool.description =
            "Measures a .titleart: PASS/WARN/FAIL per check with measured value, threshold, crops, method and a ready fix "
            "(ops for edit_title). Document checks need no GPU; pixel checks render. checks: ids or groups (default all). "
            "Passing is not aesthetic approval: show the render to a person.";
        tool.inputSchema = object({
            {"path", string("")},
            {"checks", array(string(""), "")},
            {"backgrounds", array(string(""), "")},
            {"baseline", string("PNG or rev:<n>.")},
            {"save_baseline", string("PNG path.")},
            {"fail_on", withDefault(stringEnum("", {"fail", "warn"}), "fail")},
            {"render_scale", withDefault(number("Render size of pixel checks."), 1)},
            {"screenshot", withDefault(stringEnum("", {"on_failure", "always", "never"}), "on_failure")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number(""), 120)},
        }, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.writesFiles = [](const Json& args) { return args.contains("save_baseline"); };
        tool.activity = ToolActivity::Read;
        tool.group = "title_art";
        tool.handler = titletools::checkTitleTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "export_title";
        tool.title = "Export Title";
        tool.description =
            "Bakes a .titleart's outputs (all, or `outputs` ids) to PNGs with recipes and the bake sidecar as a job; fresh ones "
            "are skipped unless force. dry_run reports files, sizes, bytes and VRAM without writing. Outputs are configured "
            "with edit_title (set outputs[i].scale). format: bake here; others arrive with later specs.";
        std::vector<std::string> formats;
        for (const FormatOwner& f : FORMATS) formats.push_back(f.format);
        tool.inputSchema = object({
            {"path", string("")},
            {"outputs", array(string(""), "")},
            {"format", withDefault(stringEnum("", formats), "bake")},
            {"options", anyObject("")},
            {"dry_run", boolean("")},
            {"force", boolean("")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number(""), 120)},
        }, {"path"});
        tool.annotations.idempotent = true;
        tool.writesFiles = [](const Json& args) { return !args.value("dry_run", false); };
        tool.activity = ToolActivity::Edit;
        tool.group = "title_art";
        tool.handler = exportTitleTool;
        registry.addTool(std::move(tool));
    }
}

}
