// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The material and environment pickers and the bands list follow Pachislop's 3D panels
// (src/ui/properties/style/Lighting3D.tsx, Material3D pickers) by SafEight, used with the author's
// permission; the look of the panels is the editor's (DocRows, ImGui).

#include "Title3DDesigner.h"

#include "Backend.h"
#include "external/IconsFontAwesome6.h"
#include "texture/Texture.h"
#include "texture/TexturePrepare.h"
#include "titleart/TitleArtHash.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/designer/TitlePages.h"
#include "titleart/designer/TitleRows.h"
#include "titleart/three_d/BevelProfile.h"
#include "titleart/three_d/Title3DEditor.h"
#include "titleart/three_d/Title3DSchema.h"
#include "titleart/three_d/Title3DStyle.h"
#include "titleart/three_d/TitleEnvironment.h"
#include "titleart/three_d/TitleMesh.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    constexpr double PI = 3.14159265358979323846;

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.is_object() ? object.find(key) : object.end();
        return object.is_object() && it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // a member as an object (null and absent read as {})
    const ta::Json& objectAt(const ta::Json& object, const char* key) {
        static const ta::Json empty = ta::Json::object();
        if (!object.is_object()) return empty;
        auto it = object.find(key);
        return it != object.end() && it->is_object() ? *it : empty;
    }

    std::vector<FieldRef> refs(const std::vector<std::string>& layers, const std::string& path) {
        std::vector<FieldRef> out;
        for (const std::string& layer : layers) out.push_back({layer, path});
        return out;
    }

    bool is3D(const ta::Json* layer) {
        return layer && ta::layerTypeHasStyle(ta::layerTypeOf(*layer)) && layer->value("dimension", std::string("2d")) == "3d";
    }

    std::string titleCase(const std::string& id) {
        std::string out;
        bool upper = true;
        for (char c : id) {
            if (c == '_') {
                out += ' ';
                upper = true;
                continue;
            }
            out += upper ? (char)std::toupper((unsigned char)c) : c;
            upper = false;
        }
        return out;
    }

    void commit(RowContext& ctx, const ta::Json& ops, const std::string& name) {
        std::string error;
        ctx.state->clearPreview();
        if (!ctx.state->commit(ops, name, &error) && ctx.host) ctx.host->reportError(error);
    }

    // a hover preview of `ops` under `id` (R54/R55: hover previews without a step)
    void preview(RowContext& ctx, const std::string& id, const ta::Json& ops, const std::string& name) {
        if (ctx.state->preview() && ctx.state->preview()->id == id) return;
        PreviewItem item;
        item.kind = PreviewItem::Kind::Effect;
        item.id = id;
        item.label = name;
        item.ops = ops;
        item.stepName = name;
        std::string error;
        ctx.state->setPreview(item, &error);
    }

    void clearPreviewWithPrefix(RowContext& ctx, const std::string& prefix) {
        if (ctx.state->preview() && ctx.state->preview()->id.rfind(prefix, 0) == 0) ctx.state->clearPreview();
    }

    // the page's node (the collect pass visits every node, not only the selection)
    bool layer3D(const TitleDesignerState& state, const NodeRef& node) {
        return node.kind == NodeKind::Layer && is3D(layerById(state.shown(), node.layer));
    }

    bool part3D(const TitleDesignerState& state, const NodeRef& node) {
        if (node.kind != NodeKind::Part || !is3D(layerById(state.shown(), node.layer))) return false;
        return node.part == "face" || node.part == "bevel" || node.part == "side" || node.part.rfind("band:", 0) == 0;
    }

    int bandIndexOf(const ta::Json& layer, const std::string& part) {
        const ta::Json& bands = objectAt(layer, "style_3d").value("bands", ta::Json::array());
        for (size_t i = 0; i < bands.size(); i++) {
            if ("band:" + str(bands[i], "id") == part) return (int)i;
        }
        return -1;
    }

    // ---- the lighting levels' inherited values (S86 R3, R51 "Inherited") ----

    ta::Json schemaDefault(const char* kindName, const std::string& member) {
        const ta::TitleArtSchema& schema = ta::TitleArtSchema::get();
        const ta::SchemaKind* kind = schema.kind(kindName);
        const ta::SchemaMember* m = kind ? schema.member(*kind, member) : nullptr;
        return m ? m->defaultValue : ta::Json();
    }

    ta::Json documentEnvironmentValue(const ta::TitleArtDoc& doc, const std::string& member) {
        const ta::Json& env = objectAt(objectAt(doc.root, "scene_3d"), "environment");
        return env.contains(member) ? env[member] : schemaDefault("environment_3d", member);
    }

    ta::Json inheritedEnvironment(const ta::TitleArtDoc& doc, const FieldRef& target, const std::string& member) {
        // a layer's own overrides take the document's; a part's take the layer's, then the document's
        if (target.path.rfind("style_3d.lighting.environment.", 0) != 0 && !target.layer.empty()) {
            if (const ta::Json* layer = layerById(doc, target.layer)) {
                const ta::Json& own = objectAt(objectAt(objectAt(*layer, "style_3d"), "lighting"), "environment");
                if (own.contains(member)) return own[member];
            }
        }
        return documentEnvironmentValue(doc, member);
    }

    ta::Json inheritedLighting(const ta::TitleArtDoc& doc, const FieldRef&, const std::string& member) {
        const ta::Json& scene = objectAt(doc.root, "scene_3d");
        return scene.contains(member) ? scene[member] : schemaDefault("scene_3d", member);
    }

    // ---- picker tiles ----

    // a material tile's document: a bevelled "Aa" in `environment` with the preset on its face
    struct TileDoc {
        std::shared_ptr<const ta::TitleArtDoc> doc;
        std::string layer;
        uint64_t content = 0;
    };
    const TileDoc& materialTileDoc(const std::string& preset, const std::string& environment) {
        static std::map<std::string, TileDoc> cache;
        const std::string key = preset + "@" + environment;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        TileDoc tile;
        ta::NewTitleArtOptions options;
        options.width = 240;
        options.height = 136;
        options.text = "Aa";
        const ta::TitleArtDoc base = ta::makeNewTitleArt(options);
        const ta::Json ops = ta::Json::array({
            ta::Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"size", 100}}}},
            ta::Json{{"op", "set_dimension"}, {"layer", "Title"}, {"dimension", "3d"}},
            ta::Json{{"op", "set"}, {"layer", "Title"}, {"values", {{"style_3d.bevel.width", 6}, {"style_3d.bevel.depth", 4}, {"style_3d.depth", 18}}}},
            ta::Json{{"op", "apply_material"}, {"layer", "Title"}, {"part", "face"}, {"preset", preset}},
            ta::Json{{"op", "set"}, {"values", {{"scene_3d.environment.preset", environment}}}},
        });
        TitleOpsOptions seed;
        seed.idSeed = 86;
        TitleOpsResult result = TitleArtOps::apply(base, ops, seed);
        if (result.ok) {
            ta::forEachLayer(result.doc.root, [&](const ta::Json& layer, const ta::Json*, int) {
                if (tile.layer.empty()) tile.layer = layer.value("id", std::string());
                return true;
            });
            tile.doc = std::make_shared<const ta::TitleArtDoc>(std::move(result.doc));
            tile.content = ta::fnv1a64(key);
        }
        return cache.emplace(key, std::move(tile)).first->second;
    }

    float toneMapped(float linear) {
        const float mapped = linear / (1.0f + linear);
        return mapped <= 0.0031308f ? mapped * 12.92f : 1.055f * std::pow(mapped, 1.0f / 2.4f) - 0.055f;
    }

    // the environment's radiance along a direction: its procedural scene, else its HDRI panorama
    struct EnvSampler {
        const ta::TitleEnvScene* scene = nullptr;
        std::shared_ptr<const ta::TitleHdri> hdri;
        float yaw = 0.0f;
        std::array<float, 3> operator()(const std::array<float, 3>& d) const {
            if (hdri && hdri->width > 0 && hdri->height > 0) {
                const double az = std::atan2(d[0], -d[2]) + yaw;
                double u = az / (2.0 * PI) + 0.5;
                u -= std::floor(u);
                const double v = std::acos(std::clamp((double)d[1], -1.0, 1.0)) / PI;
                const int x = std::clamp((int)(u * hdri->width), 0, hdri->width - 1), y = std::clamp((int)(v * hdri->height), 0, hdri->height - 1);
                const size_t i = ((size_t)y * hdri->width + x) * 4;
                std::array<float, 3> c{halfToFloat(hdri->rgba[i]), halfToFloat(hdri->rgba[i + 1]), halfToFloat(hdri->rgba[i + 2])};
                if (scene && !scene->elements.empty()) {
                    const std::array<float, 3> extra = ta::evaluateTitleEnvironment(*scene, d);
                    for (int k = 0; k < 3; k++) c[k] += extra[k];
                }
                return c;
            }
            if (scene) return ta::evaluateTitleEnvironment(*scene, d);
            return {0.5f, 0.5f, 0.5f};
        }
    };

    // the GPU textures of the open environment picker (dropped when it closes: R65)
    struct EnvTile {
        std::vector<uint8_t> pixels;
        Texture texture;
    };
    std::map<std::string, std::unique_ptr<EnvTile>>& envTiles() {
        static std::map<std::string, std::unique_ptr<EnvTile>> tiles;
        return tiles;
    }

    // ---- the pickers' open state (one at a time) ----
    std::string& openPicker() {
        static std::string id;
        return id;
    }
    std::string& pickerQuery() {
        static std::string query;
        return query;
    }
    ta::Json& materialClipboard() {
        static ta::Json clipboard;
        return clipboard;
    }

    // ---- material rows (S86 R51 Materials, R54) ----

    // the environment the layer renders with (its override, else the document's): material tiles
    // are lit by it (R54)
    std::string resolvedEnvironment(const ta::TitleArtDoc& doc, const std::string& layerId) {
        if (const ta::Json* layer = layerById(doc, layerId)) {
            const ta::Json& own = objectAt(objectAt(objectAt(*layer, "style_3d"), "lighting"), "environment");
            if (own.contains("preset") && own["preset"].is_string()) return own["preset"].get<std::string>();
        }
        const ta::Json preset = documentEnvironmentValue(doc, "preset");
        return preset.is_string() ? preset.get<std::string>() : std::string("studio");
    }

    void materialPicker(RowContext& ctx, const std::string& layerId, const std::string& base, const std::string& part) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* material = fieldValue(doc, {layerId, base});
        const std::string current = material && material->is_object() ? str(*material, "preset") : std::string("custom");
        const std::string pickerId = "mat:" + layerId + ":" + part;
        if (ctx.readOnly) ImGui::BeginDisabled();
        if (ImGui::Button((std::string(ICON_FA_CUBE " Material: ") + titleCase(current.empty() ? "custom" : current) + " " ICON_FA_CARET_DOWN "##matpick").c_str(),
                          ImVec2(-ImGui::GetFrameHeight() * 4.5f, 0.0f))) {
            ImGui::OpenPopup("##materialpicker");
            openPicker() = pickerId;
            pickerQuery().clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_COPY "##matcopy") && material) materialClipboard() = *material;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy Material");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_PASTE "##matpaste") && materialClipboard().is_object()) {
            commit(ctx, editsToOps({{layerId, base, materialClipboard()}}), "Paste Material");
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(materialClipboard().is_object() ? "Paste Material" : "Paste Material (copy one first)");
        if (ctx.readOnly) ImGui::EndDisabled();
        if (!ImGui::BeginPopup("##materialpicker")) {
            if (openPicker() == pickerId) {
                openPicker().clear();
                clearPreviewWithPrefix(ctx, "mat:");
            }
            return;
        }
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "%s", pickerQuery().c_str());
        // (a popup sizes itself to its contents: fixed widths, four tiles a row)
        ImGui::SetNextItemWidth(4.0f * 112.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputTextWithHint("##matsearch", ICON_FA_MAGNIFYING_GLASS " Search materials", buffer, sizeof(buffer))) pickerQuery() = buffer;
        std::string query = pickerQuery();
        std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        const std::string environment = resolvedEnvironment(doc, layerId);
        const float side = 104.0f;
        bool hovering = false;
        std::string section;
        int column = 0;
        const int columns = 4;
        // R54's sections, each once (the preset list interleaves them)
        std::vector<std::string> ordered;
        for (const char* wanted : {"Metals", "Gloss and Paint", "Glass and Gems", "Special"}) {
            for (const std::string& preset : ta::title3DMaterialPresetNames()) {
                if (preset != "custom" && wanted == std::string(ta::materialPresetSection(preset))) ordered.push_back(preset);
            }
        }
        ImGui::BeginChild("##materials", ImVec2(4.0f * 116.0f, 440.0f));
        for (const std::string& preset : ordered) {
            if (!query.empty() && preset.find(query) == std::string::npos && titleCase(preset).find(pickerQuery()) == std::string::npos) continue;
            const std::string presetSection = ta::materialPresetSection(preset);
            if (presetSection != section) {
                section = presetSection;
                ImGui::SeparatorText(section.c_str());
                column = 0;
            }
            if (column > 0) ImGui::SameLine();
            ImGui::PushID(preset.c_str());
            ImGui::BeginGroup();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool chosen = ImGui::InvisibleButton("##tile", ImVec2(side, side * 0.6f));
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side * 0.6f), IM_COL32(46, 46, 50, 255), 4.0f);
            const TileDoc& tile = materialTileDoc(preset, environment);
            ImTextureID texture = ctx.host && tile.doc ? ctx.host->previewTile("mat:" + preset + "@" + environment, tile.doc, tile.layer, {-120.0, -68.0, 240.0, 136.0},
                                                                               tile.content, (int)side)
                                                       : 0;
            if (texture) dl->AddImage(texture, p, ImVec2(p.x + side, p.y + side * 0.6f));
            else dl->AddText(ImVec2(p.x + side * 0.5f - 6.0f, p.y + side * 0.3f - 7.0f), IM_COL32(160, 160, 160, 255), ICON_FA_SPINNER);
            if (preset == current) dl->AddRect(p, ImVec2(p.x + side, p.y + side * 0.6f), IM_COL32(90, 160, 255, 255), 4.0f, 0, 2.0f);
            const ta::Json op = ta::Json::array({ta::Json{{"op", "apply_material"}, {"layer", "id:" + layerId}, {"part", part}, {"preset", preset}}});
            const std::string name = "Apply Material " + titleCase(preset);
            if (ImGui::IsItemHovered() && !ctx.readOnly) {
                hovering = true;
                preview(ctx, "mat:" + layerId + ":" + part + ":" + preset, op, name);
            }
            if (chosen && !ctx.readOnly) {
                commit(ctx, op, name);
                ImGui::CloseCurrentPopup();
            }
            ImGui::TextUnformatted(titleCase(preset).c_str());
            ImGui::EndGroup();
            ImGui::PopID();
            column = (column + 1) % columns;
        }
        ImGui::EndChild();
        if (!hovering) clearPreviewWithPrefix(ctx, "mat:");
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            clearPreviewWithPrefix(ctx, "mat:");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // the material's rows: picker, then its members, Surface, Base Texture, Pattern and the
    // environment overrides (inherited from the layer, then the document)
    void materialRows(RowContext& ctx, const std::string& layerId, const std::string& base, const std::string& part) {
        if (!ctx.collecting()) materialPicker(ctx, layerId, base, part);
        objectRows(ctx, {layerId}, base, "material_3d", {"preset", "link"});
        if (beginSection(ctx, "surface", "Surface", std::string(), false)) {
            const std::string saved = ctx.stepPrefix;
            ctx.stepPrefix += "Surface ";
            objectRows(ctx, {layerId}, base + ".surface", "surface_3d");
            ctx.stepPrefix = saved;
            endSection(ctx);
        }
        if (beginSection(ctx, "base_texture", "Base Texture", std::string(), false)) {
            objectRows(ctx, {layerId}, base + ".base_texture", "base_texture_3d");
            endSection(ctx);
        }
        if (beginSection(ctx, "pattern", "Pattern", std::string(), false)) {
            const std::string saved = ctx.stepPrefix;
            ctx.stepPrefix += "Pattern ";
            objectRows(ctx, {layerId}, base + ".pattern", "pattern");
            ctx.stepPrefix = saved;
            endSection(ctx);
        }
        if (beginSection(ctx, "part_environment", "Environment", std::string(), false)) {
            objectRows(ctx, {layerId}, base + ".environment", "environment_3d_overrides");
            endSection(ctx);
        }
    }

    // a nullable material ("Same as Face"): the checkbox, then its rows when it has its own
    void ownMaterialRows(RowContext& ctx, const std::string& layerId, const std::string& base, const std::string& part, const std::string& sameAs,
                         const std::string& sourceBase) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* own = fieldValue(doc, {layerId, base});
        const bool has = own && own->is_object();
        if (!ctx.collecting()) {
            bool same = !has;
            if (ctx.readOnly) ImGui::BeginDisabled();
            if (ImGui::Checkbox(("Same as " + sameAs + "##same").c_str(), &same)) {
                const ta::Json* source = fieldValue(doc, {layerId, sourceBase});
                commit(ctx, editsToOps({{layerId, base, same ? ta::Json() : (source && source->is_object() ? *source : ta::Json::object())}}),
                       same ? "Use " + sameAs + " Material" : "Own Material");
            }
            if (ctx.readOnly) ImGui::EndDisabled();
        }
        if (has) materialRows(ctx, layerId, base, part);
    }

    // ---- the bevel profile picker (R53) ----

    void drawProfile(ImDrawList* dl, ImVec2 p, ImVec2 size, const std::string& profile, const ta::Json& custom, int segments, double curve) {
        ta::three_d::BevelShape shape;
        if (!ta::three_d::bevelProfileFromName(profile, shape.profile)) shape.profile = ta::three_d::BevelProfileKind::Round;
        shape.segments = segments;
        shape.curve = curve;
        if (custom.is_object()) shape.custom = ta::decodeCurve(custom);
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(40, 40, 44, 255), 3.0f);
        const float m = 6.0f;
        const std::vector<ta::three_d::UnitPiece> pieces = ta::three_d::unitBevel(shape, segments);
        // a profile may leave the unit square (Inset, Tube): the cross-section fits its cell
        double minO = 0.0, maxO = 1.0, minH = 0.0, maxH = 1.0;
        for (const ta::three_d::UnitPiece& piece : pieces) {
            for (const ta::three_d::UnitPoint& u : piece) {
                minO = std::min(minO, u.o);
                maxO = std::max(maxO, u.o);
                minH = std::min(minH, u.h);
                maxH = std::max(maxH, u.h);
            }
        }
        for (const ta::three_d::UnitPiece& piece : pieces) {
            std::vector<ImVec2> line;
            for (const ta::three_d::UnitPoint& u : piece) {
                line.push_back(ImVec2(p.x + m + (float)((u.o - minO) / (maxO - minO)) * (size.x - 2 * m),
                                      p.y + size.y - m - (float)((u.h - minH) / (maxH - minH)) * (size.y - 2 * m)));
            }
            if (line.size() >= 2) dl->AddPolyline(line.data(), (int)line.size(), IM_COL32(230, 200, 120, 255), 0, 2.0f);
        }
    }

    void bevelProfileRows(RowContext& ctx, const std::vector<std::string>& layers, const std::string& base) {
        fieldRow(ctx, refs(layers, base + ".profile"));
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* bevel = fieldValue(doc, {layers[0], base});
        const std::string profile = bevel && bevel->is_object() ? str(*bevel, "profile") : std::string("round");
        if (!ctx.collecting()) {
            const int segments = bevel && bevel->is_object() ? bevel->value("segments", 6) : 6;
            const double curve = bevel && bevel->is_object() ? bevel->value("curve", 0.0) : 0.0;
            const ta::Json custom = bevel && bevel->is_object() ? bevel->value("custom", ta::Json()) : ta::Json();
            const std::string pickerId = "bevel:" + layers[0] + ":" + base;
            if (ctx.readOnly) ImGui::BeginDisabled();
            if (ImGui::Button(ICON_FA_BEZIER_CURVE " Profiles...##bevelpick")) {
                ImGui::OpenPopup("##bevelpicker");
                openPicker() = pickerId;
            }
            if (ctx.readOnly) ImGui::EndDisabled();
            ImGui::SameLine();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(64.0f, ImGui::GetFrameHeight()));
            drawProfile(ImGui::GetWindowDrawList(), at, ImVec2(64.0f, ImGui::GetFrameHeight()), profile, custom, segments, curve);
            if (ImGui::BeginPopup("##bevelpicker")) {
                bool hovering = false;
                int column = 0;
                for (const std::string& name : ta::three_d::bevelProfileNames()) {
                    if (column++ % 4) ImGui::SameLine();
                    ImGui::PushID(name.c_str());
                    ImGui::BeginGroup();
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const bool chosen = ImGui::InvisibleButton("##profile", ImVec2(84.0f, 52.0f));
                    drawProfile(ImGui::GetWindowDrawList(), p, ImVec2(84.0f, 52.0f), name, custom, segments, curve);
                    if (name == profile) ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + 84.0f, p.y + 52.0f), IM_COL32(90, 160, 255, 255), 3.0f, 0, 2.0f);
                    std::vector<FieldEdit> edits;
                    for (const std::string& id : layers) edits.push_back({id, base + ".profile", name});
                    const ta::Json op = editsToOps(edits);
                    if (ImGui::IsItemHovered() && !ctx.readOnly) {
                        hovering = true;
                        preview(ctx, "bevel:" + base + ":" + name, op, "Change Bevel Profile");
                    }
                    if (chosen && !ctx.readOnly) {
                        commit(ctx, op, "Change Bevel Profile");
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::TextUnformatted(titleCase(name).c_str());
                    ImGui::EndGroup();
                    ImGui::PopID();
                }
                if (!hovering) clearPreviewWithPrefix(ctx, "bevel:");
                ImGui::EndPopup();
            } else if (openPicker() == pickerId) {
                openPicker().clear();
                clearPreviewWithPrefix(ctx, "bevel:");
            }
        }
        if (profile == "custom") {
            // Custom: the Curve window over [0, 1]², with Chamfer and Round as curve presets
            fieldRow(ctx, refs(layers, base + ".custom"), {.label = "Custom Profile", .stepLabel = "Bevel Profile Curve"});
            if (!ctx.collecting() && !ctx.readOnly) {
                auto curvePreset = [&](const char* label, const std::vector<std::pair<std::pair<float, float>, bool>>& points) {
                    if (ImGui::SmallButton(label)) {
                        std::vector<FieldEdit> edits;
                        for (const std::string& id : layers) edits.push_back({id, base + ".custom", curveJsonFromKeys(points)});
                        commit(ctx, editsToOps(edits), std::string("Bevel Profile Curve ") + label);
                    }
                };
                curvePreset("Chamfer", {{{0.0f, 0.0f}, true}, {{1.0f, 1.0f}, true}});
                ImGui::SameLine();
                std::vector<std::pair<std::pair<float, float>, bool>> round;
                for (int i = 0; i <= 6; i++) {
                    const double a = (PI * 0.5) * i / 6.0;
                    round.push_back({{(float)(1.0 - std::cos(a)), (float)std::sin(a)}, false});
                }
                curvePreset("Round", round);
            }
        }
    }

    // ---- the environment picker (R55) ----

    void environmentPicker(RowContext& ctx, const std::string& layerId, const std::string& base) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const std::string pickerId = "env:" + layerId + ":" + base;
        if (ctx.readOnly) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_FA_SUN " Environments...##envpick")) {
            ImGui::OpenPopup("##envpicker");
            openPicker() = pickerId;
        }
        if (ctx.readOnly) ImGui::EndDisabled();
        if (!ImGui::BeginPopup("##envpicker")) {
            if (openPicker() == pickerId) {
                openPicker().clear();
                clearPreviewWithPrefix(ctx, "env:");
                envTiles().clear();
            }
            return;
        }
        const ta::Json* currentValue = fieldValue(doc, {layerId, base + ".preset"});
        const std::string current = currentValue && currentValue->is_string() ? currentValue->get<std::string>() : resolvedEnvironment(doc, layerId);
        bool hovering = false;
        auto setOps = [&](const ta::Json& values) {
            ta::Json op = ta::Json{{"op", "set"}, {"values", values}};
            if (!layerId.empty()) op["layer"] = "id:" + layerId;
            return ta::Json::array({op});
        };
        // one new tile per frame (CPU), the rest show a spinner until their turn
        bool madeOne = false;
        auto tile = [&](const std::string& preset) {
            ImGui::PushID(preset.c_str());
            ImGui::BeginGroup();
            const ImVec2 size(96.0f, 64.0f);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool chosen = ImGui::InvisibleButton("##env", size);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            std::unique_ptr<EnvTile>& t = envTiles()[preset];
            if (!t && !madeOne) {
                madeOne = true;
                t = std::make_unique<EnvTile>();
                t->pixels = title3DEnvironmentTile(preset, 96, 64);
                t->texture.setData("editor:titledesigner-env:" + preset, TextureData(96, 64, (unsigned int)t->pixels.size(), ColorFormat::RGBA, 4, t->pixels.data()));
                t->texture.load();
            }
            if (t && t->texture.getRender()) dl->AddImage(Backend::getImGuiTexture(t->texture.getRender()), p, ImVec2(p.x + size.x, p.y + size.y));
            else dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(46, 46, 50, 255), 4.0f);
            if (preset == current) dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), IM_COL32(90, 160, 255, 255), 4.0f, 0, 2.0f);
            const ta::Json ops = setOps(ta::Json{{base + ".preset", preset}});
            if (ImGui::IsItemHovered() && !ctx.readOnly) {
                hovering = true;
                preview(ctx, "env:" + base + ":" + preset, ops, "Change Environment");
            }
            if (chosen && !ctx.readOnly) {
                commit(ctx, ops, "Change Environment");
                ImGui::CloseCurrentPopup();
            }
            ImGui::TextUnformatted(titleCase(preset).c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        };
        auto section = [&](const char* title, bool photo) {
            ImGui::SeparatorText(title);
            int column = 0;
            for (const std::string& preset : ta::title3DEnvironmentNames()) {
                if (preset == "custom" || preset == "matcap") continue;
                if (ta::titleEnvironmentIsPhoto(preset) != photo) continue;
                if (column++ % 4) ImGui::SameLine();
                tile(preset);
            }
        };
        section("Studio", false);
        section("Photo", true);
        ImGui::SeparatorText("Project HDRIs");
        const std::vector<std::string> hdris = ctx.host ? ctx.host->projectFiles({".hdr", ".exr"}) : std::vector<std::string>();
        if (hdris.empty()) ImGui::TextDisabled("Import an .hdr or .exr into the project, or drop one here");
        for (const std::string& file : hdris) {
            const ta::Json ops = setOps(ta::Json{{base + ".preset", "custom"}, {base + ".hdri", ta::Json{{"path", file}, {"guid", ""}}}});
            if (ImGui::Selectable((std::string(ICON_FA_IMAGE " ") + file).c_str()) && !ctx.readOnly) {
                commit(ctx, ops, "Change Environment");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered() && !ctx.readOnly) {
                hovering = true;
                preview(ctx, "env:" + base + ":" + file, ops, "Change Environment");
            }
        }
        // an .hdr / .exr dragged from the Project window
        ImGui::Dummy(ImVec2(4.0f * 104.0f, 24.0f));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                const std::string text((const char*)payload->Data, (size_t)payload->DataSize);
                std::string first = text.substr(0, text.find('\n'));
                while (!first.empty() && first.back() == '\0') first.pop_back();
                commit(ctx, setOps(ta::Json{{base + ".preset", "custom"}, {base + ".hdri", ta::Json{{"path", first}, {"guid", ""}}}}), "Change Environment");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDragDropTarget();
        }
        if (!hovering) clearPreviewWithPrefix(ctx, "env:");
        ImGui::EndPopup();
    }

    // ---- lights ----

    // a lights list at `path` ("scene_3d.lights", "style_3d.lighting.lights") of `layerId`
    void lightsList(RowContext& ctx, const std::string& layerId, const std::string& path) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* lights = fieldValue(doc, {layerId, path});
        const ta::Json list = lights && lights->is_array() ? *lights : ta::Json::array();
        for (size_t i = 0; i < list.size(); i++) {
            const std::string at = path + "[" + std::to_string(i) + "]";
            if (!ctx.collecting()) {
                ImGui::PushID((int)i);
                ImGui::SeparatorText(("Light " + std::to_string(i + 1)).c_str());
            }
            const std::string saved = ctx.stepPrefix;
            ctx.stepPrefix = "Light ";
            for (const char* key : {"type", "color", "intensity"}) fieldRow(ctx, {{layerId, at + "." + key}});
            ctx.stepPrefix = saved;
            if (!ctx.collecting()) {
                // the position: three numbers, one step per drag
                const ta::Json position = list[i].value("position", ta::Json::array({-600, 800, 1000}));
                float xyz[3] = {0, 0, 0};
                for (int k = 0; k < 3 && k < (int)position.size(); k++) xyz[k] = position[(size_t)k].is_number() ? position[(size_t)k].get<float>() : 0.0f;
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Position");
                ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
                ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() * 1.4f);
                if (ctx.readOnly) ImGui::BeginDisabled();
                const bool changed = ImGui::DragFloat3("##position", xyz, 4.0f);
                if (ImGui::IsItemActivated()) ctx.state->beginGesture("Change Light Position");
                std::string error;
                if (changed) ctx.state->stage(editsToOps({{layerId, at + ".position", ta::Json::array({xyz[0], xyz[1], xyz[2]})}}), &error);
                if (ImGui::IsItemDeactivated()) ctx.state->endGesture(&error);
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_TRASH "##remove")) {
                    ta::Json rest = list;
                    rest.erase(rest.begin() + (long)i);
                    commit(ctx, editsToOps({{layerId, path, rest}}), "Remove Light");
                }
                if (ctx.readOnly) ImGui::EndDisabled();
                if (!error.empty() && ctx.host) ctx.host->reportError(error);
                ImGui::PopID();
            }
        }
        if (!ctx.collecting() && !ctx.readOnly && list.size() < 8 && ImGui::Button(ICON_FA_PLUS " Light", ImVec2(-1.0f, 0.0f))) {
            ta::Json more = list;
            more.push_back(ta::Json{{"type", "point"}, {"color", "#ffffff"}, {"intensity", 2}, {"position", ta::Json::array({0, 400, 800})}});
            commit(ctx, editsToOps({{layerId, path, more}}), "Add Light");
        }
        if (!ctx.collecting() && ta::countNonAmbientLights(ta::decodeLights3D(list)) > ta::TITLE_3D_MAX_LIGHTS) {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " More than %d lights: the extra ones are not drawn", ta::TITLE_3D_MAX_LIGHTS);
        }
    }

    // ---- the Dimension subsections (R51) ----

    void geometrySection(RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* first = layerById(doc, layers[0]);
        if (!first) return;
        // (copies: a row or a picker's hover can replace the shown document)
        const std::string mode = str(objectAt(*first, "style_3d"), "mode");
        const ta::LayerType type = ta::layerTypeOf(*first);
        for (const char* key : {"mode", "depth", "curve_segments", "skew"}) fieldRow(ctx, refs(layers, std::string("style_3d.") + key));
        if (beginSection(ctx, "3d_bevel", "Bevel", std::string(), true)) {
            ctx.stepPrefix = "Bevel ";
            fieldRow(ctx, refs(layers, "style_3d.bevel_position"), {.label = "Position", .stepLabel = "Bevel Position"});
            objectRows(ctx, layers, "style_3d.bevel", "bevel_3d", {"profile", "custom"});
            bevelProfileRows(ctx, layers, "style_3d.bevel");
            ctx.stepPrefix.clear();
            endSection(ctx);
        }
        if (mode == "inflate" && beginSection(ctx, "3d_inflate", "Inflate", std::string(), true)) {
            ctx.stepPrefix = "Inflate ";
            objectRows(ctx, layers, "style_3d.inflate", "inflate_3d");
            ctx.stepPrefix.clear();
            endSection(ctx);
        }
        if (type == ta::LayerType::Image || type == ta::LayerType::Vector) {
            fieldRow(ctx, refs(layers, "style_3d.face_image"), {.label = "Face Image"});
            fieldRow(ctx, refs(layers, "style_3d.edge_image"), {.label = "Edge Image"});
        }
    }

    void bandsList(RowContext& ctx, const std::string& layerId) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* layer = layerById(doc, layerId);
        if (!layer || ctx.collecting()) return;
        const ta::Json bands = objectAt(*layer, "style_3d").value("bands", ta::Json::array());
        if (bands.empty()) ImGui::TextDisabled("No bands: a band is a ring around the letters, like a rim");
        for (size_t i = 0; i < bands.size(); i++) {
            const ta::Json& band = bands[i];
            const std::string part = "band:" + str(band, "id");
            ImGui::PushID((int)i);
            bool enabled = band.value("enabled", true);
            if (ctx.readOnly) ImGui::BeginDisabled();
            if (ImGui::Checkbox("##on", &enabled)) {
                commit(ctx, editsToOps({{layerId, "style_3d.bands[" + std::to_string(i) + "].enabled", enabled}}), enabled ? "Enable Band" : "Disable Band");
            }
            if (ctx.readOnly) ImGui::EndDisabled();
            ImGui::SameLine();
            ta::TitleColor c{0.75f, 0.75f, 0.75f};
            ta::parseTitleColor(objectAt(band, "material").value("color", ta::Json("#c0c0c0")), c);
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            const float s = ImGui::GetFrameHeight() * 0.7f;
            ImGui::Dummy(ImVec2(s, ImGui::GetFrameHeight()));
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(sp.x, sp.y + (ImGui::GetFrameHeight() - s) * 0.5f), ImVec2(sp.x + s, sp.y + (ImGui::GetFrameHeight() + s) * 0.5f),
                                                      ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)), 2.0f);
            ImGui::SameLine();
            const NodeRef node{NodeKind::Part, layerId, part, {}};
            const std::string name = "Band " + std::to_string(i + 1) + (str(band, "name").empty() ? std::string() : "  " + str(band, "name"));
            const std::string summary = titleCase(str(objectAt(band, "material"), "preset")) + " \xC2\xB7 grow " + formatValue(band.value("grow", ta::Json(3)), ta::ParamUnit::Percent);
            if (ImGui::Selectable((name + "   " + summary + "##band").c_str(), ctx.state->isSelected(node)) && ctx.host) ctx.host->selectNode(node);
            // drag to reorder: one move_band step
            if (!ctx.readOnly && ImGui::BeginDragDropSource()) {
                const int index = (int)i;
                ImGui::SetDragDropPayload("titleart_band", &index, sizeof(index));
                ImGui::TextUnformatted(name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("titleart_band")) {
                    const int from = *(const int*)payload->Data;
                    if (from != (int)i) {
                        commit(ctx, ta::Json::array({ta::Json{{"op", "move_band"}, {"layer", "id:" + layerId}, {"band", "band#" + std::to_string(from)}, {"index", (int)i}}}),
                               "Move Band");
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##bandmenu")) {
                if (ImGui::MenuItem("Duplicate", nullptr, false, !ctx.readOnly && bands.size() < ta::TITLE_3D_MAX_BANDS)) {
                    ta::Json values = ta::Json::object();
                    for (auto it = band.begin(); it != band.end(); ++it) {
                        if (it.key() != "id") values[it.key()] = it.value();
                    }
                    commit(ctx, ta::Json::array({ta::Json{{"op", "add_band"}, {"layer", "id:" + layerId}, {"index", (int)i + 1}, {"values", values}}}), "Duplicate Band");
                }
                if (ImGui::MenuItem("Remove", nullptr, false, !ctx.readOnly)) {
                    commit(ctx, ta::Json::array({ta::Json{{"op", "remove_band"}, {"layer", "id:" + layerId}, {"band", "band#" + std::to_string(i)}}}), "Remove Band");
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (!ctx.readOnly && bands.size() < ta::TITLE_3D_MAX_BANDS) {
            if (ImGui::Button(ICON_FA_PLUS " Add Band " ICON_FA_CARET_DOWN, ImVec2(-1.0f, 0.0f))) ImGui::OpenPopup("##addband");
            if (ImGui::BeginPopup("##addband")) {
                if (ImGui::MenuItem("Band")) commit(ctx, ta::Json::array({ta::Json{{"op", "add_band"}, {"layer", "id:" + layerId}}}), "Add Band");
                ImGui::Separator();
                for (const std::string& preset : title3DBandPresetNames()) {
                    if (ImGui::MenuItem(titleCase(preset).c_str())) {
                        commit(ctx, ta::Json::array({ta::Json{{"op", "add_band"}, {"layer", "id:" + layerId}, {"preset", preset}}}), "Add Band " + titleCase(preset));
                    }
                }
                ImGui::EndPopup();
            }
        }
    }

    void lightingSection(RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        const std::string layerId = layers[0];
        if (!ctx.collecting()) environmentPicker(ctx, layerId, "style_3d.lighting.environment");
        ctx.stepPrefix = "Environment ";
        objectRows(ctx, layers, "style_3d.lighting.environment", "environment_3d_overrides");
        ctx.stepPrefix.clear();
        fieldRow(ctx, refs(layers, "style_3d.env_lock"));
        fieldRow(ctx, refs(layers, "style_3d.lighting.tone_mapping"));
        fieldRow(ctx, refs(layers, "style_3d.lighting.exposure"));
        if (beginSection(ctx, "3d_lights", "Lights", std::string(), false)) {
            // (after the rows above: they can replace the shown document)
            const ta::TitleArtDoc& doc = ctx.state->shown();
            const ta::Json* layer = layerById(doc, layerId);
            const ta::Json& lighting = layer ? objectAt(objectAt(*layer, "style_3d"), "lighting") : objectAt(ta::Json(), "");
            if (lighting.contains("lights")) {
                if (!ctx.collecting() && !ctx.readOnly && ImGui::SmallButton("Inherit Lights")) {
                    ta::Json rest = lighting;
                    rest.erase("lights");
                    commit(ctx, editsToOps({{layerId, "style_3d.lighting", rest}}), "Inherit Lights");
                }
                lightsList(ctx, layerId, "style_3d.lighting.lights");
            } else if (!ctx.collecting()) {
                const ta::Json scene = objectAt(doc.root, "scene_3d").value("lights", schemaDefault("scene_3d", "lights"));
                ImGui::TextDisabled("Inherited: %zu light%s of the document", scene.size(), scene.size() == 1 ? "" : "s");
                if (!ctx.readOnly && ImGui::SmallButton("Override Lights")) commit(ctx, editsToOps({{layerId, "style_3d.lighting.lights", scene}}), "Override Lights");
            }
            endSection(ctx);
        }
    }

    void cameraSection(RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* camera = fieldValue(doc, {layers[0], "style_3d.camera"});
        const bool own = camera && camera->is_object();
        if (!ctx.collecting()) {
            bool useDocument = !own;
            if (ctx.readOnly) ImGui::BeginDisabled();
            if (ImGui::Checkbox("Use Document Camera", &useDocument)) {
                const ta::Json scene = objectAt(objectAt(doc.root, "scene_3d"), "camera");
                std::vector<FieldEdit> edits;
                for (const std::string& id : layers) edits.push_back({id, "style_3d.camera", useDocument ? ta::Json() : scene});
                commit(ctx, editsToOps(edits), useDocument ? "Use Document Camera" : "Own Camera");
            }
            if (ctx.readOnly) ImGui::EndDisabled();
        }
        if (own) {
            ctx.stepPrefix = "Camera ";
            objectRows(ctx, layers, "style_3d.camera", "camera_3d");
            ctx.stepPrefix.clear();
        }
    }

    void previewSection(RowContext& ctx, const std::vector<std::string>&, const std::vector<std::string>&) {
        if (ctx.collecting() || !ctx.host) return;
        // it renders the canvas as that tier would, without changing the document
        const std::string mode = ctx.host->previewMode();
        static const char* const tiers[][2] = {{"realtime", "Bake"}, {"realtime_high", "High"}, {"realtime_medium", "Medium"}, {"realtime_low", "Low"}};
        for (size_t i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(tiers[i][1], mode == tiers[i][0])) ctx.host->choosePreviewMode(tiers[i][0]);
        }
        ImGui::TextDisabled("The canvas as that Realtime tier renders it (view only)");
    }

    // the Status footer (R57): mesh stats at the tier shown, a fit badge per tier, diagnostics;
    // measured once the document has been still for 0.5 s while the section is open
    void statusSection(RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        if (ctx.collecting()) return;
        struct Stats {
            const TitleDesignerState* state = nullptr;
            uint64_t change = ~0ull;
            ta::Json rows;
            double changedAt = 0.0;
            uint64_t seen = ~0ull;
        };
        static Stats stats;
        const double t = ImGui::GetTime();
        if (stats.seen != ctx.state->changeCount() || stats.state != ctx.state) {
            stats.seen = ctx.state->changeCount();
            stats.state = ctx.state;
            stats.changedAt = t;
        }
        if ((stats.change != ctx.state->changeCount() || stats.rows.is_null()) && t - stats.changedAt >= 0.5 && !ctx.state->gestureActive()) {
            stats.change = ctx.state->changeCount();
            stats.rows = title3DStats(ctx.state->shown(), ctx.host ? ctx.host->assets() : nullptr, ta::TitleTier::Bake);
        }
        if (stats.change != ctx.state->changeCount() || !stats.rows.is_array()) {
            ImGui::TextDisabled(ICON_FA_SPINNER " Building mesh...");
            return;
        }
        for (const ta::Json& row : stats.rows) {
            if (str(row, "layer") != layers[0]) continue;
            ImGui::Text("%d triangles \xC2\xB7 %d vertices \xC2\xB7 %d solids", row.value("triangles", 0), row.value("vertices", 0), row.value("solids", 0));
            ImGui::TextDisabled("build %.1f ms \xC2\xB7 k %d \xC2\xB7 environment %d px", row.value("build_ms", 0.0), row.value("k", 1), row.value("environment_face", 0));
            const ta::Json& tiers = objectAt(row, "tiers");
            for (const char* tier : {"high", "medium", "low"}) {
                const ta::Json& fit = objectAt(tiers, tier);
                const bool fits = fit.value("fits", true);
                std::string over;
                for (const ta::Json& o : fit.value("over", ta::Json::array())) over += (over.empty() ? "" : ", ") + (o.is_string() ? o.get<std::string>() : o.dump());
                ImGui::TextColored(fits ? ImVec4(0.45f, 0.76f, 0.47f, 1.0f) : ImVec4(0.95f, 0.65f, 0.2f, 1.0f), "%s: %s", titleCase(tier).c_str(),
                                   fits ? "fits" : ("over (" + over + ")").c_str());
            }
            if (row.value("fallback_used", false)) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " The mesher used its fallback for overlapping outlines");
        }
        // a missing HDRI keeps the studio (R57)
        const ta::Json* layer = layerById(ctx.state->shown(), layers[0]);
        const std::string env = resolvedEnvironment(ctx.state->shown(), layers[0]);
        if (layer && ta::titleEnvironmentIsPhoto(env) && !ta::loadTitleHdri(ta::titleEnvironmentPreset(env) ? ta::titleEnvironmentPreset(env)->hdri : env)) {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " The %s HDRI is missing: the studio lights it", titleCase(env).c_str());
        }
    }

    // ---- the 3D Appearance (replaces S82's 2D Appearance on a 3D layer) ----

    void appearanceSection(RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        if (ctx.collecting() || layers.size() != 1) return;
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* layer = layerById(doc, layers[0]);
        if (!layer) return;
        const ta::Json& style = objectAt(*layer, "style_3d");
        if (!layer->contains("style_3d") || !(*layer)["style_3d"].is_object()) {
            ImGui::TextDisabled("Its 3D style is derived from the 2D one");
            if (!ctx.readOnly && ImGui::SmallButton("Make 3D Style")) {
                commit(ctx, ta::Json::array({ta::Json{{"op", "set_dimension"}, {"layer", "id:" + layers[0]}, {"dimension", "3d"}}}), "Set Dimension 3D");
            }
            return;
        }
        auto partRow = [&](const std::string& key, const std::string& name, const ta::Json& material) {
            const NodeRef node{NodeKind::Part, layers[0], key, {}};
            ImGui::PushID(key.c_str());
            ta::TitleColor c{0.75f, 0.75f, 0.75f};
            ta::parseTitleColor(material.value("color", ta::Json("#c0c0c0")), c);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float s = ImGui::GetFrameHeight() * 0.7f;
            ImGui::Dummy(ImVec2(s, ImGui::GetFrameHeight()));
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + (ImGui::GetFrameHeight() - s) * 0.5f), ImVec2(p.x + s, p.y + (ImGui::GetFrameHeight() + s) * 0.5f),
                                                      ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)), 2.0f);
            ImGui::SameLine();
            const std::string preset = str(material, "preset");
            if (ImGui::Selectable((name + "   " + titleCase(preset.empty() ? "custom" : preset) + "##part").c_str(), ctx.state->isSelected(node)) && ctx.host) {
                ctx.host->selectNode(node);
            }
            ImGui::PopID();
        };
        const ta::Json& face = objectAt(style, "face");
        partRow("face", "Face", face);
        partRow("bevel", "Bevel", style.contains("bevel_material") && style["bevel_material"].is_object() ? style["bevel_material"] : face);
        partRow("side", "Side", style.contains("side") && style["side"].is_object() ? style["side"] : face);
        ImGui::Separator();
        bandsList(ctx, layers[0]);
    }

    // ---- part pages: Face, Bevel, Side, Band N (R51 Materials, R52) ----

    void partSection(RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>& parts) {
        if (layers.empty() || parts.empty()) return;
        const std::string& layerId = layers[0];
        const std::string& part = parts[0];
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* layer = layerById(doc, layerId);
        if (!layer) return;
        if (part == "face") {
            ctx.stepPrefix = "Face ";
            materialRows(ctx, layerId, "style_3d.face", "face");
        } else if (part == "bevel") {
            ctx.stepPrefix = "Bevel ";
            ownMaterialRows(ctx, layerId, "style_3d.bevel_material", "bevel", "Face", "style_3d.face");
        } else if (part == "side") {
            ctx.stepPrefix = "Side ";
            ownMaterialRows(ctx, layerId, "style_3d.side", "side", "Face", "style_3d.face");
        } else {
            const int index = bandIndexOf(*layer, part);
            if (index < 0) return;
            const std::string base = "style_3d.bands[" + std::to_string(index) + "]";
            const std::string prefix = "Band " + std::to_string(index + 1) + " ";
            ctx.stepPrefix = prefix;
            for (const char* key : {"enabled", "name", "grow", "join", "fill_counters", "depth", "offset"}) fieldRow(ctx, {{layerId, base + "." + key}});
            if (beginSection(ctx, "band_plate", "Plate", std::string(), false)) {
                ctx.stepPrefix = prefix + "Plate ";
                objectRows(ctx, {layerId}, base + ".plate", "plate");
                endSection(ctx);
            }
            if (beginSection(ctx, "band_bevel", "Bevel", std::string(), false)) {
                ctx.stepPrefix = prefix + "Bevel ";
                objectRows(ctx, {layerId}, base + ".bevel", "bevel_3d", {"profile", "custom"});
                bevelProfileRows(ctx, {layerId}, base + ".bevel");
                endSection(ctx);
            }
            if (beginSection(ctx, "band_material", "Material", std::string(), true)) {
                ctx.stepPrefix = prefix;
                materialRows(ctx, layerId, base + ".material", "band#" + std::to_string(index));
                endSection(ctx);
            }
            if (beginSection(ctx, "band_side", "Side", std::string(), false)) {
                ctx.stepPrefix = prefix + "Side ";
                ownMaterialRows(ctx, layerId, base + ".side", "band#" + std::to_string(index), "Band", base + ".material");
                endSection(ctx);
            }
        }
        ctx.stepPrefix.clear();
    }

    // ---- the Document page's 3D Scene (R51) ----

    void sceneSection(RowContext& ctx, const std::vector<std::string>&, const std::vector<std::string>&) {
        const std::vector<std::string> document = {std::string()};
        if (beginSection(ctx, "scene_camera", "Camera", std::string(), false)) {
            ctx.stepPrefix = "Camera ";
            objectRows(ctx, document, "scene_3d.camera", "camera_3d");
            ctx.stepPrefix.clear();
            endSection(ctx);
        }
        if (beginSection(ctx, "scene_environment", "Environment", std::string(), true)) {
            if (!ctx.collecting()) environmentPicker(ctx, std::string(), "scene_3d.environment");
            ctx.stepPrefix = "Environment ";
            objectRows(ctx, document, "scene_3d.environment", "environment_3d");
            ctx.stepPrefix.clear();
            endSection(ctx);
        }
        if (beginSection(ctx, "scene_lights", "Lights", std::string(), false)) {
            lightsList(ctx, std::string(), "scene_3d.lights");
            endSection(ctx);
        }
        for (const char* key : {"tone_mapping", "exposure", "supersample"}) fieldRow(ctx, {{std::string(), std::string("scene_3d.") + key}});
    }

    bool documentHas3D(const TitleDesignerState& state) {
        const ta::TitleArtDoc& doc = state.shown();
        if (doc.root.contains("scene_3d") && doc.root["scene_3d"].is_object()) return true;
        return false;
    }

}

std::vector<uint8_t> jasperengine::editor::title3DEnvironmentTile(const std::string& preset, int width, int height) {
    std::vector<uint8_t> out((size_t)width * height * 4, 0);
    EnvSampler sample;
    sample.scene = ta::titleEnvironmentPreset(preset);
    const std::string slug = sample.scene && !sample.scene->hdri.empty() ? sample.scene->hdri : ta::titleEnvironmentIsPhoto(preset) ? preset : std::string();
    if (!slug.empty()) sample.hdri = ta::loadTitleHdri(slug);
    // a photo environment whose HDRI is not installed renders the studio (R57): its tile says so
    if (!slug.empty() && !sample.hdri) sample.scene = ta::titleEnvironmentPreset("studio");
    sample.yaw = ta::titleHdriYaw(preset);
    auto put = [&](int x, int y, const std::array<float, 3>& c, float alpha) {
        uint8_t* p = &out[((size_t)y * width + x) * 4];
        for (int k = 0; k < 3; k++) p[k] = (uint8_t)std::clamp((int)std::lround(toneMapped(std::max(0.0f, c[k])) * 255.0f), 0, 255);
        p[3] = (uint8_t)std::clamp((int)std::lround(alpha * 255.0f), 0, 255);
    };
    // the panorama strip along the bottom quarter, the background above it dark
    const int strip = std::max(4, height / 4);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (y >= height - strip) {
                const double az = ((double)x / width - 0.5) * 2.0 * PI;
                const double el = (0.5 - (double)(y - (height - strip)) / strip) * PI;
                put(x, y, sample({(float)(std::cos(el) * std::sin(az)), (float)std::sin(el), (float)(-std::cos(el) * std::cos(az))}), 1.0f);
            } else {
                uint8_t* p = &out[((size_t)y * width + x) * 4];
                p[0] = p[1] = p[2] = 30;
                p[3] = 255;
            }
        }
    }
    // a chrome ball (mirror reflection) and a gold disc (a rough gold: the reflection blurred
    // toward the normal, tinted)
    const int top = height - strip;
    const float radius = std::min(width * 0.25f, top * 0.45f);
    const float cy = top * 0.5f;
    for (int ball = 0; ball < 2; ball++) {
        const float cx = width * (ball == 0 ? 0.27f : 0.73f);
        for (int y = 0; y < top; y++) {
            for (int x = 0; x < width; x++) {
                const float nx = (x + 0.5f - cx) / radius, ny = (cy - (y + 0.5f)) / radius;
                const float r2 = nx * nx + ny * ny;
                if (r2 > 1.0f) continue;
                const float nz = std::sqrt(1.0f - r2);
                // the view looks down -z: the reflection of (0, 0, -1) about the normal
                const std::array<float, 3> r{2.0f * nz * nx, 2.0f * nz * ny, 2.0f * nz * nz - 1.0f};
                std::array<float, 3> c = sample(r);
                if (ball == 1) {
                    const std::array<float, 3> d = sample({nx, ny, nz});
                    const float gold[3] = {1.0f, 0.78f, 0.34f};
                    for (int k = 0; k < 3; k++) c[k] = (0.55f * c[k] + 0.45f * d[k]) * gold[k];
                }
                // a soft edge
                put(x, y, c, std::clamp((1.0f - std::sqrt(r2)) * radius, 0.0f, 1.0f));
                uint8_t* p = &out[((size_t)y * width + x) * 4];
                const float a = p[3] / 255.0f;
                for (int k = 0; k < 3; k++) p[k] = (uint8_t)(p[k] * a + 30 * (1.0f - a));
                p[3] = 255;
            }
        }
    }
    return out;
}

bool jasperengine::editor::title3DGizmoTarget(const ta::TitleArtDoc& doc, const std::string& layerId, const Gizmo3DLetters* letters, Gizmo3DTarget& out) {
    const ta::Json* layer = layerById(doc, layerId);
    if (!is3D(layer) || layer->value("locked", false)) return false;
    // the layer's parent space in the document
    Affine2 parentToDoc;
    std::vector<std::string> chain;
    for (std::string p = parentLayerOf(doc, layerId); !p.empty(); p = parentLayerOf(doc, p)) chain.push_back(p);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (const ta::Json* parent = layerById(doc, *it)) parentToDoc = parentToDoc * layerTransformAffine(ta::decodeTransform(parent->value("transform", ta::Json::object())));
    }
    const ta::Json& transform = objectAt(*layer, "transform");
    const ta::TitleTransform t = ta::decodeTransform(transform);
    const double z = transform.value("z", 0.0);
    const Vec3d rotation{transform.value("rotation_x", 0.0), transform.value("rotation_y", 0.0), t.rotation};
    const double size = ta::layerSourceSize(*layer);
    if (!letters) {
        const TitleVec2 origin = parentToDoc.apply(t.position);
        out = layerGizmo3DTarget(layerId, Vec3d{origin.x, origin.y, z}, Vec3d{t.position.x, t.position.y, z}, rotation, std::max(0.01, size * 0.01));
        const TitleVec2 ax = parentToDoc.applyLinear({1.0, 0.0}), ay = parentToDoc.applyLinear({0.0, 1.0});
        out.axes = {Vec3d{ax.x, ax.y, 0.0}, Vec3d{ay.x, ay.y, 0.0}, Vec3d{0.0, 0.0, 1.0}};
        out.moveStep = "Move 3D Layer";
        out.rotateStep = "Rotate 3D Layer";
        return true;
    }
    if (letters->indexes.empty()) return false;
    // the selected letters: offset (% of size, y up) and dz, tilt_x / tilt_y / rotation inside the layer
    const Affine2 layerToDoc = parentToDoc * layerTransformAffine(t);
    const ta::Json& chars = objectAt(*layer, "chars");
    struct Start {
        int index;
        double ox, oy, dz, tx, ty, rot;
    };
    std::vector<Start> starts;
    for (int index : letters->indexes) {
        const ta::Json& adjust = objectAt(chars, std::to_string(index).c_str());
        const ta::TitleVec2 offset = adjust.contains("offset") ? ta::decodeVec2(adjust["offset"]) : ta::TitleVec2{};
        starts.push_back({index, offset.x, offset.y, adjust.value("dz", 0.0), adjust.value("tilt_x", 0.0), adjust.value("tilt_y", 0.0), adjust.value("rotation", 0.0)});
    }
    const Start& primary = starts.front();
    out = Gizmo3DTarget();
    out.id = layerId;
    out.origin = Vec3d{letters->centre.x, letters->centre.y, z};
    const double unit = size * 0.01;
    const TitleVec2 ax = layerToDoc.applyLinear({unit, 0.0}), ay = layerToDoc.applyLinear({0.0, unit});
    out.axes = {Vec3d{ax.x, ax.y, 0.0}, Vec3d{ay.x, ay.y, 0.0}, Vec3d{0.0, 0.0, unit}};
    out.rotation = RotationModel::glyph(RotationModel::layer().orient(rotation));
    out.pos = Vec3d{primary.ox, primary.oy, primary.dz};
    out.rot = Vec3d{primary.tx, primary.ty, primary.rot};
    out.unit = "%";
    out.step = 1.0;
    out.moveStep = "Move Letters";
    out.rotateStep = "Tilt Letters";
    out.write = [layerId, starts](const Vec3d& dPos, const Vec3d& dRot, Gizmo3DGesture gesture) {
        // every selected letter from its own value at the press
        std::vector<FieldEdit> edits;
        for (const Start& s : starts) {
            const std::string base = "chars." + std::to_string(s.index) + ".";
            if (gesture == Gizmo3DGesture::Move) {
                edits.push_back({layerId, base + "offset", ta::Json::array({s.ox + dPos.x, s.oy + dPos.y})});
                edits.push_back({layerId, base + "dz", s.dz + dPos.z});
            } else {
                edits.push_back({layerId, base + "tilt_x", s.tx + dRot.x});
                edits.push_back({layerId, base + "tilt_y", s.ty + dRot.y});
                edits.push_back({layerId, base + "rotation", s.rot + dRot.z});
            }
        }
        return edits;
    };
    return true;
}

void jasperengine::editor::registerTitle3DDesigner() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    // the ops the rows and pickers commit (S86 R59)
    registerTitle3DEditor();

    // the lighting levels: a layer's and a part's environment overrides, a layer's tone mapping and
    // exposure (R3, R51 "Inherited")
    InheritanceRule environment;
    environment.pattern = "environment_3d_overrides.*";
    environment.sourceName = "the layer or the document";
    environment.sparse = true;
    environment.valueAt = inheritedEnvironment;
    registerInheritance(environment);
    for (const char* member : {"tone_mapping", "exposure"}) {
        InheritanceRule lighting;
        lighting.pattern = std::string("lighting_3d.") + member;
        lighting.sourceName = "the document";
        lighting.sparse = true;
        lighting.valueAt = inheritedLighting;
        registerInheritance(lighting);
    }

    // the Dimension section's subsections, on Dimension 3D layers
    auto sub = [](const char* id, const char* title, std::function<void(RowContext&, const std::vector<std::string>&, const std::vector<std::string>&)> draw,
                  bool open = false) {
        SectionDef def;
        def.defaultOpen = open;
        def.page = SectionDef::Page::Layer;
        def.id = id;
        def.title = title;
        def.parent = "dimension";
        def.appliesTo = layer3D;
        def.draw = std::move(draw);
        addSection(std::move(def));
    };
    sub("3d_geometry", "Geometry", geometrySection, true);
    sub("3d_bands", "Bands", [](RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        if (layers.size() == 1) bandsList(ctx, layers[0]);
    }, true);
    sub("3d_lighting", "Lighting", lightingSection);
    sub("3d_glint", "Glint", [](RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        ctx.stepPrefix = "Glint ";
        objectRows(ctx, layers, "style_3d.glint", "glint_3d");
        ctx.stepPrefix.clear();
    });
    sub("3d_ao", "Ambient Occlusion", [](RowContext& ctx, const std::vector<std::string>& layers, const std::vector<std::string>&) {
        ctx.stepPrefix = "Ambient Occlusion ";
        objectRows(ctx, layers, "style_3d.ao", "ao_3d");
        ctx.stepPrefix.clear();
    });
    sub("3d_camera", "Camera", cameraSection);
    sub("3d_preview", "Realtime Preview", previewSection);
    sub("3d_status", "Status", statusSection);

    SectionDef appearance;
    appearance.page = SectionDef::Page::Layer;
    appearance.id = "3d_appearance";
    appearance.title = "Appearance";
    appearance.replaces = "appearance";
    appearance.appliesTo = layer3D;
    appearance.draw = appearanceSection;
    addSection(std::move(appearance));

    SectionDef part;
    part.page = SectionDef::Page::Part;
    part.id = "3d_part";
    part.title = "Material";
    part.replaces = "material";
    part.appliesTo = part3D;
    part.draw = partSection;
    addSection(std::move(part));

    SectionDef scene;
    scene.page = SectionDef::Page::Document;
    scene.id = "3d_scene";
    scene.title = "3D Scene";
    scene.applies = documentHas3D;
    scene.draw = sceneSection;
    scene.summary = [](const TitleDesignerState& state) {
        const ta::Json env = documentEnvironmentValue(state.shown(), "preset");
        return titleCase(env.is_string() ? env.get<std::string>() : std::string("studio"));
    };
    addSection(std::move(scene));

    // Preview As: the Realtime tiers (R51 Realtime Preview)
    const std::pair<const char*, ta::TitleTier> tiers[] = {{"high", ta::TitleTier::High}, {"medium", ta::TitleTier::Medium}, {"low", ta::TitleTier::Low}};
    for (const auto& [name, tier] : tiers) {
        PreviewModeDef mode;
        mode.id = std::string("realtime_") + name;
        mode.label = "Realtime " + titleCase(name);
        mode.note = "3D layers as the " + titleCase(name) + " Realtime tier renders them";
        mode.renderTier = (int)tier;
        addPreviewMode(std::move(mode));
    }

    setGizmo3DProvider([](const TitleDesignerState& state, const std::string& layerId, const Gizmo3DLetters* letters, Gizmo3DTarget& out) {
        return title3DGizmoTarget(state.shown(), layerId, letters, out);
    });
}
