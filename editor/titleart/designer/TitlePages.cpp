// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The Appearance rows (one line per part with its summary and the most-changed number) and the
// section summaries follow Pachislop's AppearanceRow and GroupTitle (src/ui/properties/) by
// SafEight, used with the author's permission.

#include "TitlePages.h"

#include "profiler/Profile.h"
#include "titleart/TitleArtIO.h"
#include "titleart/fx/FxGeneratorChecks.h"
#include "titleart/fx/FxPresets.h"
#include "widget/ColorField.h"

#include "external/IconsFontAwesome6.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <random>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    std::vector<SectionDef>& sections() {
        static std::vector<SectionDef> list;
        return list;
    }

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    const char* const MATERIAL_SECTIONS[][2] = {{"fill", "Fill"},           {"bevel", "Bevel"},   {"reflection", "Reflection"},
                                                {"finish", "Finish"},       {"pattern", "Pattern"}, {"satin", "Satin"},
                                                {"texture", "Texture"},     {"inner_glow", "Inner Glow"}, {"gloss", "Gloss"},
                                                {"glass", "Glass"},         {"sheen", "Sheen"}};

    std::vector<FieldRef> refs(const std::vector<std::string>& layers, const std::string& path) {
        std::vector<FieldRef> out;
        for (const std::string& layer : layers) out.push_back({layer, path});
        return out;
    }

    std::string join(const std::vector<std::string>& parts) {
        std::string out;
        for (const std::string& p : parts) {
            if (p.empty()) continue;
            if (!out.empty()) out += " \xC2\xB7 ";
            out += p;
        }
        return out;
    }

    // the schema kind of a material member ("material.bevel" -> "bevel")
    // a member by reference (the pages read big objects: a layer's style with its rims), the
    // fallback when absent or of another type
    const ta::Json& emptyObject() {
        static const ta::Json value = ta::Json::object();
        return value;
    }
    const ta::Json& emptyArray() {
        static const ta::Json value = ta::Json::array();
        return value;
    }
    const ta::Json& memberOr(const ta::Json& object, const char* key, const ta::Json& fallback) {
        if (!object.is_object()) return fallback;
        auto it = object.find(key);
        return it != object.end() && it->type() == fallback.type() ? *it : fallback;
    }

    std::string memberKind(const std::string& kindName, const std::string& key) {
        const ta::TitleArtSchema& schema = ta::TitleArtSchema::get();
        const ta::SchemaKind* kind = schema.kind(kindName);
        if (!kind) return {};
        const ta::SchemaMember* member = schema.member(*kind, key);
        return member ? member->kind : std::string();
    }

    bool sectionApplies(const RowContext& ctx, const SectionDef& def) {
        if (def.appliesTo) return def.appliesTo(*ctx.state, ctx.node);
        return !def.applies || def.applies(*ctx.state);
    }

    void drawRegistered(RowContext& ctx, const SectionDef& def, const std::vector<std::string>& layers, const std::vector<std::string>& parts,
                        bool defaultOpen) {
        const std::string summary = def.summary && !ctx.collecting() ? def.summary(*ctx.state) : std::string();
        if (beginSection(ctx, "reg_" + def.id, def.title, summary, defaultOpen)) {
            def.draw(ctx, layers, parts);
            endSection(ctx);
        }
    }

    void registeredSections(RowContext& ctx, SectionDef::Page page, const std::vector<std::string>& layers, const std::vector<std::string>& parts) {
        for (const SectionDef& def : sections()) {
            if (def.page != page || !def.draw || !def.parent.empty() || !def.replaces.empty()) continue;
            if (!sectionApplies(ctx, def)) continue;
            drawRegistered(ctx, def, layers, parts, def.defaultOpen);
        }
    }

    // the sections registered inside S82's section `parent` (§6.8)
    void childSections(RowContext& ctx, SectionDef::Page page, const std::string& parent, const std::vector<std::string>& layers,
                       const std::vector<std::string>& parts) {
        for (const SectionDef& def : sections()) {
            if (def.page != page || !def.draw || def.parent != parent) continue;
            if (!sectionApplies(ctx, def)) continue;
            drawRegistered(ctx, def, layers, parts, def.defaultOpen);
        }
    }

    // a registered section that takes the place of S82's section `id` for this selection
    const SectionDef* replacingSection(RowContext& ctx, SectionDef::Page page, const std::string& id) {
        for (const SectionDef& def : sections()) {
            if (def.page != page || !def.draw || def.replaces != id) continue;
            if (!sectionApplies(ctx, def)) continue;
            return &def;
        }
        return nullptr;
    }

    // a material (face, a rim's, a character's face): its sub-objects as sections
    void materialSections(RowContext& ctx, const std::vector<std::string>& layers, const std::string& base, const std::string& stepPrefix) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* material = fieldValue(doc, {layers[0], base});
        const std::string savedPrefix = ctx.stepPrefix;
        for (const auto& section : MATERIAL_SECTIONS) {
            const std::string key = section[0];
            const std::string kind = memberKind("material", key);
            const ta::Json* object = material && material->is_object() && material->contains(key) ? &(*material)[key] : nullptr;
            // (a collect pass never shows a summary)
            const std::string summary = object && !ctx.collecting() ? objectSummary(kind, *object) : std::string();
            // material groups open by default only if something in them is on (R19)
            const bool on = key == "fill" || (object && object->value("enabled", false)) ||
                            (object && object->contains("kind") && str(*object, "kind") != "none" && key != "fill");
            ctx.stepPrefix = stepPrefix + (key == "fill" ? std::string() : std::string(section[1]) + " ");
            if (beginSection(ctx, "mat_" + key, section[1], summary, on)) {
                objectRows(ctx, layers, base + "." + key, kind);
                if (key == "fill") {
                    const ta::Json* fill = object;
                    const std::string fillKind = fill ? str(*fill, "kind") : std::string("solid");
                    if (fillKind != "solid" && fillKind != "image" && fillKind != "source") {
                        fieldRow(ctx, refs(layers, base + ".fill.gradient"), {.label = "Gradient"});
                    }
                    if (fillKind == "image") {
                        if (beginSection(ctx, "fill_image", "Image", std::string(), true)) {
                            objectRows(ctx, layers, base + ".fill.image", "fill_image");
                            endSection(ctx);
                        }
                    }
                }
                if (key == "bevel") {
                    fieldRow(ctx, refs(layers, base + ".bevel.profile_curve"), {.label = "Profile Curve"});
                    fieldRow(ctx, refs(layers, base + ".bevel.gloss_curve"), {.label = "Gloss Curve"});
                }
                if (key == "satin") fieldRow(ctx, refs(layers, base + ".satin.contour"), {.label = "Contour"});
                if (key == "finish") fieldRow(ctx, refs(layers, base + ".finish.gradient"), {.label = "Gradient"});
                endSection(ctx);
            }
        }
        ctx.stepPrefix = savedPrefix;
        fieldRow(ctx, refs(layers, base + ".opacity"));
        fieldRow(ctx, refs(layers, base + ".refraction"));
    }

    // the effects of a stack as compact cards: eye, name, summary; a click opens its page
    void effectCards(RowContext& ctx, const std::string& layer, const std::string& part) {
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* object = layerById(doc, layer);
        if (!object) return;
        const ta::Json* list = nullptr;
        if (part.empty()) {
            auto it = object->find("effects");
            if (it != object->end()) list = &*it;
        } else {
            auto parts = object->find("part_effects");
            if (parts != object->end() && parts->contains(part)) list = &(*parts)[part];
        }
        const size_t count = list && list->is_array() ? list->size() : 0;
        if (ctx.collecting()) {
            // each effect's rows are found under its own page (collectIndex visits every effect)
            return;
        }
        ImGui::TextDisabled("Applied top to bottom");
        for (size_t i = 0; i < count; i++) {
            const ta::Json& effect = (*list)[i];
            const std::string type = str(effect, "type");
            const ta::EffectDef* def = ta::findEffect(type);
            NodeRef node{NodeKind::Effect, layer, part, str(effect, "id")};
            ImGui::PushID((int)i);
            bool enabled = effect.value("enabled", true);
            if (ctx.readOnly) ImGui::BeginDisabled();
            if (ImGui::Checkbox("##on", &enabled)) {
                std::string error;
                const std::string path = effectPathOf(*object, node) + ".enabled";
                ctx.state->commit(editsToOps({{layer, path, enabled}}), std::string(enabled ? "Enable" : "Disable") + " Effect " +
                                                                           (def ? def->label : type),
                                  &error);
                if (!error.empty() && ctx.host) ctx.host->reportError(error);
            }
            if (ctx.readOnly) ImGui::EndDisabled();
            ImGui::SameLine();
            const std::string label = std::string(def ? def->label : type.c_str()) + (def && !def->gizmos.empty() ? "  \xE2\x9C\xA5" : "");
            CostBadge badge;
            const bool costed = ctx.host && ctx.host->effectCost(layer, part, (int)i, badge);
            const bool live = def && def->live.kind != ta::LiveKind::None && ctx.host;
            const float right = (costed ? ImGui::CalcTextSize(badge.amber ? "C4 Baked on mobile" : "C0").x + 8.0f : 0.0f) +
                                (live ? ImGui::GetFrameHeight() + 4.0f : 0.0f);
            if (ImGui::Selectable((label + "##fx").c_str(), ctx.state->isSelected(node), 0, ImVec2(ImGui::GetContentRegionAvail().x - right, 0.0f)) && ctx.host) {
                ctx.host->selectNode(node);
            }
            if (ImGui::IsItemHovered() && def) ImGui::SetTooltip("%s", def->description);
            if (costed) {
                ImGui::SameLine();
                drawCostBadge(badge);
            }
            if (live) {
                // S83 D10's live variant played on the canvas (view state, never saved)
                ImGui::SameLine();
                const std::string id = str(effect, "id");
                const bool on = ctx.host->livePreviewOn(layer, id);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                if (ImGui::SmallButton(on ? ICON_FA_STOP "##live" : ICON_FA_PLAY "##live")) ctx.host->setLivePreview(layer, id, !on);
                if (on) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    std::string params;
                    for (const char* p : def->live.params) params += (params.empty() ? "" : ", ") + std::string(p);
                    ImGui::SetTooltip("Preview Live: plays %s on the canvas as a Realtime title animates them without re-rendering "
                                      "(view only, not saved)",
                                      params.c_str());
                }
            }
            if (!def) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unknown effect type '%s' (kept as it is)", type.c_str());
            }
            ImGui::PopID();
        }
        if (!ctx.readOnly && ImGui::Button(ICON_FA_PLUS " Add Effect", ImVec2(-1.0f, 0.0f)) && ctx.host) ctx.host->openAddPalette("effects");
    }

    // S83 R39: an effect's or generator's presets as chips over its rows; a chip sets every
    // param to the preset's value over the type's defaults (one step "Apply Preset <name>"),
    // hovering it previews that (no step); a seed param gets the dice ("Randomize Seed")
    void presetChips(RowContext& ctx, const std::string& layer, const std::string& base, const std::string& type, const std::vector<ta::ParamDef>& params) {
        if (ctx.collecting()) return;
        const std::vector<TitleFxPreset>& presets = TitleFxPresets::forType(type);
        bool hovering = false;
        auto valuesOf = [&](const TitleFxPreset& preset) {
            std::vector<FieldEdit> edits;
            for (const ta::ParamDef& param : params) {
                auto it = preset.params.find(param.key);
                edits.push_back({layer, base + "." + param.key, it != preset.params.end() ? *it : param.defaultValue});
            }
            return editsToOps(edits);
        };
        if (!presets.empty()) {
            ImGui::TextDisabled("Presets");
            float x = ImGui::GetItemRectMax().x;
            const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            if (ctx.readOnly) ImGui::BeginDisabled();
            for (size_t i = 0; i < presets.size(); i++) {
                const TitleFxPreset& preset = presets[i];
                const float w = ImGui::CalcTextSize(preset.name.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                if (x + ImGui::GetStyle().ItemSpacing.x + w < right) ImGui::SameLine();
                ImGui::PushID((int)i);
                if (ImGui::SmallButton(preset.name.c_str())) {
                    ctx.state->clearPreview();
                    std::string error;
                    ctx.state->commit(valuesOf(preset), "Apply Preset " + preset.name, &error);
                    if (!error.empty() && ctx.host) ctx.host->reportError(error);
                }
                if (ImGui::IsItemHovered() && !ctx.readOnly) {
                    hovering = true;
                    const std::string id = "preset:" + layer + ":" + base + ":" + preset.id;
                    if (!ctx.state->preview() || ctx.state->preview()->id != id) {
                        PreviewItem item;
                        item.kind = PreviewItem::Kind::Effect;
                        item.id = id;
                        item.label = preset.name;
                        item.ops = valuesOf(preset);
                        item.stepName = "Apply Preset " + preset.name;
                        std::string error;
                        ctx.state->setPreview(item, &error);
                    }
                }
                x = ImGui::GetItemRectMax().x;
                ImGui::PopID();
            }
            if (ctx.readOnly) ImGui::EndDisabled();
        }
        // leaving the chips restores the document (R42's transient state)
        if (!hovering && ctx.state->preview() && ctx.state->preview()->id.rfind("preset:" + layer + ":" + base + ":", 0) == 0) ctx.state->clearPreview();
        for (const ta::ParamDef& param : params) {
            if (std::string(param.key) != "seed") continue;
            if (ctx.readOnly) ImGui::BeginDisabled();
            if (ImGui::SmallButton(ICON_FA_DICE " Randomize Seed")) {
                static std::mt19937 random(std::random_device{}());
                const int64_t hi = param.max > param.min ? (int64_t)param.max : 2147483647;
                std::uniform_int_distribution<int64_t> pick(param.max > param.min ? (int64_t)param.min : 0, hi);
                std::string error;
                ctx.state->commit(editsToOps({{layer, base + ".seed", pick(random)}}), "Randomize Seed", &error);
                if (!error.empty() && ctx.host) ctx.host->reportError(error);
            }
            if (ctx.readOnly) ImGui::EndDisabled();
        }
    }

    // a following generator's Follow Layer (S83 R43): the layer picker, and the row warning
    // "Follows a deleted layer" with Pick Layer when its layer is gone
    void followRow(RowContext& ctx, const std::string& layerId, const ta::GeneratorDef& def) {
        if (ctx.collecting()) {
            fieldRow(ctx, {{layerId, "follow"}}, {.label = "Follow Layer"});
            return;
        }
        const ta::TitleArtDoc& doc = ctx.state->shown();
        const ta::Json* layer = layerById(doc, layerId);
        if (!layer) return;
        const std::string follow = str(*layer, "follow");
        const ta::Json* followed = follow.empty() ? nullptr : layerById(doc, follow);
        bool missing = false;
        for (const TitleFollowProblem& p : titleFollowProblems(doc)) missing = missing || p.layerId == layerId;
        if (missing) {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s",
                               follow.empty() && def.follow == ta::FollowMode::Required ? "Follows no layer: it draws nothing" : "Follows a deleted layer");
            ImGui::SameLine();
            if (!ctx.readOnly && ImGui::SmallButton("Pick Layer")) ImGui::OpenPopup("##followpick");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Follow Layer");
        ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ctx.readOnly) ImGui::BeginDisabled();
        const std::string current = followed ? str(*followed, "name") : follow.empty() ? std::string("None") : std::string("(deleted layer)");
        if (ImGui::BeginCombo("##follow", current.c_str())) {
            auto choose = [&](const std::string& id, const std::string& name) {
                std::string error;
                ctx.state->commit(editsToOps({{layerId, "follow", id}}), id.empty() ? std::string("Unfollow Layer") : "Follow " + name, &error);
                if (!error.empty() && ctx.host) ctx.host->reportError(error);
            };
            if (def.follow == ta::FollowMode::Optional && ImGui::Selectable("None", follow.empty())) choose(std::string(), std::string());
            ta::forEachLayer(doc.root, [&](const ta::Json& other, const ta::Json*, int) {
                if (!ta::layerTypeHasStyle(ta::layerTypeOf(other))) return true;
                const std::string id = str(other, "id");
                if (ImGui::Selectable((str(other, "name") + "##" + id).c_str(), id == follow)) choose(id, str(other, "name"));
                return true;
            });
            ImGui::EndCombo();
        }
        if (ImGui::BeginPopup("##followpick")) {
            ImGui::TextDisabled("Follow which layer?");
            ta::forEachLayer(doc.root, [&](const ta::Json& other, const ta::Json*, int) {
                if (!ta::layerTypeHasStyle(ta::layerTypeOf(other))) return true;
                const std::string id = str(other, "id");
                if (ImGui::Selectable((str(other, "name") + "##p" + id).c_str())) {
                    std::string error;
                    ctx.state->commit(editsToOps({{layerId, "follow", id}}), "Follow " + str(other, "name"), &error);
                    if (!error.empty() && ctx.host) ctx.host->reportError(error);
                }
                return true;
            });
            ImGui::EndPopup();
        }
        if (ctx.readOnly) ImGui::EndDisabled();
    }

    std::string rimSummary(const ta::Json& rim) {
        std::vector<std::string> parts;
        parts.push_back(formatValue(rim.value("width", ta::Json(3)), ta::ParamUnit::Percent));
        const ta::Json material = rim.value("material", ta::Json::object());
        const ta::Json fill = material.value("fill", ta::Json::object());
        const std::string kind = fill.value("kind", std::string("solid"));
        parts.push_back(kind == "solid" ? fill.value("color", std::string("#ffffff")) : enumLabel(kind));
        if (material.value("bevel", ta::Json::object()).value("enabled", false)) parts.push_back("bevel");
        return join(parts);
    }

}

void jasperengine::editor::titledesigner::addSection(SectionDef section) {
    removeSection(section.id);
    auto& list = sections();
    auto at = list.end();
    if (!section.after.empty()) {
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->id == section.after) at = it + 1;
        }
    }
    list.insert(at, std::move(section));
}

void jasperengine::editor::titledesigner::removeSection(const std::string& id) {
    auto& list = sections();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const SectionDef& s) { return s.id == id; }), list.end());
}

std::string jasperengine::editor::titledesigner::typographySummary(const ta::Json& layer) {
    std::vector<std::string> parts;
    const double tracking = layer.value("tracking", 0.0);
    if (tracking != 0.0) parts.push_back("Tracking " + formatValue(ta::Json(tracking), ta::ParamUnit::None));
    if (layer.value("vertical", false)) parts.push_back("Vertical");
    const double h = layer.value("h_scale", 1.0), v = layer.value("v_scale", 1.0);
    if (h != 1.0 || v != 1.0) parts.push_back("Scale " + formatValue(ta::Json(h), ta::ParamUnit::None) + " x " + formatValue(ta::Json(v), ta::ParamUnit::None));
    if (layer.value("shear", 0.0) != 0.0) parts.push_back("Slant " + formatValue(layer["shear"], ta::ParamUnit::Deg));
    return join(parts);
}

std::string jasperengine::editor::titledesigner::transformSummary(const ta::Json& layer) {
    const ta::TitleTransform t = ta::decodeTransform(layer.value("transform", ta::Json::object()));
    std::vector<std::string> parts;
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "X %.0f  Y %.0f", t.position.x, t.position.y);
    parts.push_back(buffer);
    if (t.rotation != 0.0) parts.push_back(formatValue(ta::Json(t.rotation), ta::ParamUnit::Deg));
    if (t.scale != 1.0) {
        std::snprintf(buffer, sizeof(buffer), "\xC3\x97%.2g", t.scale);
        parts.push_back(buffer);
    }
    return join(parts);
}

void jasperengine::editor::titledesigner::layerHeaderRows(RowContext& ctx, const std::vector<std::string>& layers) {
    if (layers.size() == 1) fieldRow(ctx, refs(layers, "name"), {.label = "Name", .stepLabel = "Layer Name"});
    fieldRow(ctx, refs(layers, "blend"), {.label = "Blend"});
    fieldRow(ctx, refs(layers, "opacity"), {.label = "Opacity"});
}

void jasperengine::editor::titledesigner::documentPage(RowContext& ctx) {
    JE_PROFILE_SCOPE("TD documentPage");
    const ta::TitleArtDoc& doc = ctx.state->shown();
    const std::vector<std::string> documentTarget = {std::string()};
    ctx.nodeKind = "document";
    ctx.countNoun.clear();
    const std::string canvasSummary = std::to_string(doc.canvasWidth()) + " \xC3\x97 " + std::to_string(doc.canvasHeight());
    if (beginSection(ctx, "canvas", "Canvas", canvasSummary, true)) {
        fieldRow(ctx, refs(documentTarget, "name"), {.label = "Title Name"});
        objectRows(ctx, documentTarget, "canvas", "canvas");
        ctx.stepPrefix = "Background ";
        objectRows(ctx, documentTarget, "canvas.background", "canvas_background");
        ctx.stepPrefix.clear();
        endSection(ctx);
    }
    const ta::Json light = doc.root.value("global_light", ta::Json::object());
    if (beginSection(ctx, "light", "Global Light",
                     formatValue(light.value("angle", ta::Json(120)), ta::ParamUnit::Deg) + " \xC2\xB7 " +
                         formatValue(light.value("altitude", ta::Json(30)), ta::ParamUnit::Deg),
                     false)) {
        ctx.stepPrefix = "Global Light ";
        objectRows(ctx, documentTarget, "global_light", "global_light");
        ctx.stepPrefix.clear();
        endSection(ctx);
    }
    if (beginSection(ctx, "seed", "Seed", formatValue(doc.root.value("seed", ta::Json(1)), ta::ParamUnit::None), false)) {
        fieldRow(ctx, refs(documentTarget, "seed"), {.label = "Seed"});
        endSection(ctx);
    }
    const ta::Json outputs = doc.root.value("outputs", ta::Json::array());
    if (beginSection(ctx, "outputs", "Outputs", std::to_string(outputs.size()), false)) {
        for (size_t i = 0; i < outputs.size(); i++) {
            const std::string id = str(outputs[i], "id");
            if (beginSection(ctx, "output_" + std::to_string(i), "Output " + id, objectSummary("output", outputs[i]), i == 0)) {
                ctx.stepPrefix = "Output ";
                objectRows(ctx, documentTarget, "outputs[" + std::to_string(i) + "]", "output");
                ctx.stepPrefix.clear();
                endSection(ctx);
            }
        }
        endSection(ctx);
    }
    const ta::Json exposed = doc.root.value("exposed", ta::Json::array());
    if (beginSection(ctx, "exposed", "Exposed Parameters", std::to_string(exposed.size()), false)) {
        if (!ctx.collecting()) {
            if (exposed.empty()) ImGui::TextDisabled("None: use a row's menu > Expose as Parameter...");
            for (size_t i = 0; i < exposed.size(); i++) {
                const std::string name = str(exposed[i], "name");
                ImGui::PushID((int)i);
                ImGui::BulletText("%s  (%s, %zu target%s)", name.c_str(), str(exposed[i], "type").c_str(),
                                  exposed[i].value("targets", ta::Json::array()).size(),
                                  exposed[i].value("targets", ta::Json::array()).size() == 1 ? "" : "s");
                ImGui::SameLine();
                if (!ctx.readOnly && ImGui::SmallButton("Unexpose")) {
                    std::string error;
                    ctx.state->commit(ta::Json::array({ta::Json{{"op", "unexpose"}, {"name", name}}}), "Unexpose Parameter " + name, &error);
                    if (!error.empty() && ctx.host) ctx.host->reportError(error);
                }
                ImGui::PopID();
            }
        }
        for (size_t i = 0; i < exposed.size(); i++) {
            ctx.stepPrefix = "Parameter ";
            objectRows(ctx, documentTarget, "exposed[" + std::to_string(i) + "]", "exposed", {"name", "type", "targets", "options"});
            ctx.stepPrefix.clear();
        }
        endSection(ctx);
    }
    registeredSections(ctx, SectionDef::Page::Document, {}, {});
}

void jasperengine::editor::titledesigner::layerPage(RowContext& ctx, const std::vector<std::string>& layers) {
    JE_PROFILE_SCOPE("TD layerPage");
    const ta::TitleArtDoc& doc = ctx.state->shown();
    if (layers.empty()) return;
    const ta::Json* first = layerById(doc, layers[0]);
    if (!first) return;
    ta::LayerType type = ta::layerTypeOf(*first);
    bool sameType = true;
    for (const std::string& id : layers) {
        const ta::Json* l = layerById(doc, id);
        if (!l || ta::layerTypeOf(*l) != type) sameType = false;
    }
    const std::string typeName = ta::layerTypeName(type);
    ctx.nodeKind = sameType ? typeName : std::string("layers");
    ctx.countNoun = "Layers";
    ctx.stepPrefix.clear();
    layerHeaderRows(ctx, layers);

    if (beginSection(ctx, "transform", "Transform", transformSummary(*first), true)) {
        objectRows(ctx, layers, "transform", "transform");
        endSection(ctx);
    }
    if (!sameType || type == ta::LayerType::Unknown) {
        if (!ctx.collecting() && !sameType) ImGui::TextDisabled("%zu layers of different types: only the sections they share", layers.size());
        registeredSections(ctx, SectionDef::Page::Layer, layers, {});
        return;
    }
    const ta::SchemaKind* base = ta::TitleArtSchema::get().kind("layer");
    const ta::SchemaKind* variant = base ? ta::TitleArtSchema::get().variantOf(*base, *first) : nullptr;
    const std::string variantName = variant ? variant->name : std::string("layer");

    switch (type) {
        case ta::LayerType::Text: {
            if (beginSection(ctx, "text", "Text", std::string(), true)) {
                for (const char* key : {"text", "size", "align", "language"}) fieldRow(ctx, refs(layers, key));
                const std::string text = first->value("text", std::string());
                if (!ctx.collecting() && text.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " Text is empty: this layer draws nothing");
                }
                endSection(ctx);
            }
            if (beginSection(ctx, "font", "Font", first->value("font", ta::Json()).is_object() ? first->value("font", ta::Json()).value("path", std::string()) : std::string("Built-in"), true)) {
                if (!ctx.collecting() && ctx.host) ctx.host->drawFontRow(layers, ctx.readOnly);
                if (ctx.collecting() && ctx.sink) {
                    IndexEntry entry;
                    entry.rowId = layers[0] + "|layer|font";
                    entry.label = "Font";
                    entry.aliases = {"typeface", "family", "フォント", "書体"};
                    entry.location = ctx.sectionChain;
                    entry.node = ctx.node;
                    entry.nodeLabel = ctx.nodeLabel;
                    entry.targets = refs(layers, "font");
                    entry.fieldPath = "font";
                    entry.kind = "font";  // its live control is the window's font row (PropertyHost::drawFontRow)
                    ctx.sink->add(std::move(entry));
                }
                endSection(ctx);
            }
            // Selected Characters (R38): the S80 CharAdjust of the characters selected on the canvas
            std::string editing;
            std::vector<int> selectedChars;
            if (!ctx.collecting() && ctx.host && ctx.host->selectedCharacters(editing, selectedChars) && editing == layers[0] && !selectedChars.empty()) {
                const std::string title = selectedChars.size() == 1 ? "Selected Character" : std::to_string(selectedChars.size()) + " Selected Characters";
                if (beginSection(ctx, "selectedchars", title, std::string(), true)) {
                    const std::string savedNoun = ctx.countNoun;
                    ctx.countNoun = "Letters";
                    ctx.stepPrefix = "Letter ";
                    for (const char* key : {"offset", "scale", "h_scale", "v_scale", "rotation"}) {
                        std::vector<FieldRef> targets;
                        for (int index : selectedChars) targets.push_back({layers[0], "chars." + std::to_string(index) + "." + key});
                        fieldRow(ctx, targets);
                    }
                    ctx.stepPrefix.clear();
                    ctx.countNoun = savedNoun;
                    endSection(ctx);
                }
            }
            if (beginSection(ctx, "typography", "Typography", typographySummary(*first), false)) {
                for (const char* key : {"tracking", "line_height", "kerning", "vertical", "tate_chu_yoko", "tate_chu_yoko_max", "proportional",
                                        "h_scale", "v_scale", "shear"}) {
                    fieldRow(ctx, refs(layers, key));
                }
                endSection(ctx);
            }
            break;
        }
        case ta::LayerType::Shape:
            if (beginSection(ctx, "shape", "Shape", enumLabel(first->value("shape", std::string("rect"))), true)) {
                objectRows(ctx, layers, "", variantName, {"weight"});
                if (first->value("shape", std::string()) == "speech" && beginSection(ctx, "tail", "Tail", std::string(), true)) {
                    ctx.stepPrefix = "Tail ";
                    objectRows(ctx, layers, "tail", "shape_tail");
                    ctx.stepPrefix.clear();
                    endSection(ctx);
                }
                endSection(ctx);
            }
            break;
        case ta::LayerType::Image:
            if (beginSection(ctx, "image", "Image", std::string(), true)) {
                objectRows(ctx, layers, "", variantName);
                if (beginSection(ctx, "trace", "Trace", objectSummary("trace", first->value("trace", ta::Json::object())), false)) {
                    ctx.stepPrefix = "Trace ";
                    objectRows(ctx, layers, "trace", "trace");
                    ctx.stepPrefix.clear();
                    endSection(ctx);
                }
                endSection(ctx);
            }
            break;
        case ta::LayerType::Vector:
            if (beginSection(ctx, "vector", "Vector", enumLabel(first->value("mode", std::string("shape"))), true)) {
                objectRows(ctx, layers, "", variantName, {"weight"});
                endSection(ctx);
            }
            break;
        case ta::LayerType::Group:
            if (beginSection(ctx, "group", "Group", std::to_string(ta::layerChildren(*first).size()) + " layers", true)) {
                objectRows(ctx, layers, "", variantName);
                endSection(ctx);
            }
            break;
        case ta::LayerType::Generator: {
            const ta::Json gen = first->value("generator", ta::Json::object());
            const ta::GeneratorDef* def = ta::findGenerator(str(gen, "type"));
            if (beginSection(ctx, "generator", def ? def->label : "Generator", def ? def->category : std::string(), true)) {
                if (!ctx.collecting() && def) ImGui::TextWrapped("%s", def->description);
                if (def && layers.size() == 1) {
                    CostBadge badge;
                    if (!ctx.collecting() && ctx.host && ctx.host->effectCost(layers[0], std::string(), -1, badge)) {
                        ImGui::TextDisabled("Cost");
                        ImGui::SameLine();
                        drawCostBadge(badge);
                    }
                    presetChips(ctx, layers[0], "generator.params", def->type, def->params);
                }
                if (def && def->follow != ta::FollowMode::None && layers.size() == 1) followRow(ctx, layers[0], *def);
                if (def) paramRows(ctx, layers, "generator.params", def->params, nullptr, def);
                if (!def && !ctx.collecting()) ImGui::TextDisabled("Unknown generator '%s' (kept as it is)", str(gen, "type").c_str());
                endSection(ctx);
            }
            break;
        }
        case ta::LayerType::Adjustment: {
            const ta::Json adj = first->value("adjustment", ta::Json::object());
            const ta::SchemaKind* adjBase = ta::TitleArtSchema::get().kind("adjustment");
            const ta::SchemaKind* adjVariant = adjBase ? ta::TitleArtSchema::get().variantOf(*adjBase, adj) : nullptr;
            if (beginSection(ctx, "adjustment", "Adjustment", enumLabel(str(adj, "type")), true)) {
                if (adjVariant) objectRows(ctx, layers, "adjustment", adjVariant->name);
                endSection(ctx);
            }
            break;
        }
        default: break;
    }

    if (ta::layerTypeHasStyle(type)) {
        // Appearance: Face and each rim as one row (R19)
        // (collecting reads in place; drawing copies: a button may commit and replace the document)
        const ta::Json styleCopy = ctx.collecting() ? ta::Json() : first->value("style", ta::Json::object());
        const ta::Json& style = ctx.collecting() ? memberOr(*first, "style", emptyObject()) : styleCopy;
        const ta::Json rimsCopy = ctx.collecting() ? ta::Json() : style.value("rims", ta::Json::array());
        const ta::Json& rims = ctx.collecting() ? memberOr(style, "rims", emptyArray()) : rimsCopy;
        std::string appearance = std::to_string(rims.size()) + (rims.size() == 1 ? " rim" : " rims");
        if (const SectionDef* replaced = replacingSection(ctx, SectionDef::Page::Layer, "appearance")) {
            drawRegistered(ctx, *replaced, layers, {}, true);
        } else if (beginSection(ctx, "appearance", "Appearance", appearance, true)) {
            if (!ctx.collecting()) {
                const float width = ImGui::GetContentRegionAvail().x;
                auto partRow = [&](const std::string& key, const std::string& name, const std::string& summary, const ta::Json* material, int rimIndex) {
                    NodeRef node{NodeKind::Part, layers[0], key, {}};
                    ImGui::PushID(key.c_str());
                    // swatch of its fill
                    ta::TitleColor c;
                    const ta::Json fill = material ? material->value("fill", ta::Json::object()) : ta::Json::object();
                    ta::parseTitleColor(fill.value("color", ta::Json("#ffffff")), c);
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    const float s = ImGui::GetFrameHeight() * 0.7f;
                    ImGui::Dummy(ImVec2(s, ImGui::GetFrameHeight()));
                    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + (ImGui::GetFrameHeight() - s) * 0.5f),
                                                              ImVec2(p.x + s, p.y + (ImGui::GetFrameHeight() + s) * 0.5f),
                                                              ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, 1.0f)), 2.0f);
                    ImGui::SameLine();
                    if (material && !str(*material, "link").empty()) {
                        ImGui::TextDisabled(ICON_FA_LINK);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Linked material: changes reach every part linked to it");
                        ImGui::SameLine();
                    }
                    const std::string text = name + "   " + summary;
                    const float numberWidth = rimIndex >= 0 ? width * 0.28f : 0.0f;
                    if (ImGui::Selectable((text + "##part").c_str(), ctx.state->isSelected(node), 0, ImVec2(width - s - numberWidth - 24.0f, 0)) && ctx.host) {
                        ctx.host->selectNode(node);
                    }
                    if (rimIndex >= 0) {
                        // the rim width on the row
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(numberWidth);
                        ImGui::BeginGroup();
                        RowContext sub = ctx;
                        sub.stepPrefix = "Rim ";
                        sub.countNoun = "Rims";
                        ImGui::PushItemWidth(numberWidth);
                        ImGui::BeginChild("##w", ImVec2(numberWidth, ImGui::GetFrameHeight()), false, ImGuiWindowFlags_NoScrollbar);
                        fieldRow(sub, refs(layers, "style.rims[" + std::to_string(rimIndex) + "].width"), {.label = " ", .stepLabel = "Rim Width"});
                        ImGui::EndChild();
                        ImGui::PopItemWidth();
                        ImGui::EndGroup();
                        if (ImGui::BeginPopupContextItem("##rimmenu")) {
                            std::string error;
                            if (ImGui::MenuItem("Move Up", nullptr, false, rimIndex > 0 && !ctx.readOnly)) {
                                ctx.state->commit(ta::Json::array({ta::Json{{"op", "move_rim"}, {"layer", "id:" + layers[0]}, {"rim", "rim#" + std::to_string(rimIndex)},
                                                                    {"index", rimIndex - 1}}}),
                                                  "Move Rim", &error);
                            }
                            if (ImGui::MenuItem("Move Down", nullptr, false, rimIndex + 1 < (int)rims.size() && !ctx.readOnly)) {
                                ctx.state->commit(ta::Json::array({ta::Json{{"op", "move_rim"}, {"layer", "id:" + layers[0]}, {"rim", "rim#" + std::to_string(rimIndex)},
                                                                    {"index", rimIndex + 1}}}),
                                                  "Move Rim", &error);
                            }
                            if (ImGui::MenuItem("Duplicate", nullptr, false, !ctx.readOnly && rims.size() < ta::TITLE_MAX_RIMS)) {
                                ta::Json values = ta::Json::object();
                                const ta::Json& rim = rims[(size_t)rimIndex];
                                for (auto it = rim.begin(); it != rim.end(); ++it) {
                                    if (it.key() != "id") values[it.key()] = it.value();
                                }
                                ctx.state->commit(ta::Json::array({ta::Json{{"op", "add_rim"}, {"layer", "id:" + layers[0]}, {"values", values}}}), "Duplicate Rim", &error);
                            }
                            if (ImGui::MenuItem("Remove", nullptr, false, !ctx.readOnly)) {
                                ctx.state->commit(ta::Json::array({ta::Json{{"op", "remove_rim"}, {"layer", "id:" + layers[0]}, {"rim", "rim#" + std::to_string(rimIndex)}}}),
                                                  "Remove Rim", &error);
                            }
                            ImGui::EndPopup();
                            if (!error.empty() && ctx.host) ctx.host->reportError(error);
                        }
                    }
                    ImGui::PopID();
                };
                const ta::Json face = style.value("face", ta::Json::object());
                partRow("face", "Face", objectSummary("fill", face.value("fill", ta::Json::object())), &face, -1);
                for (size_t i = 0; i < rims.size(); i++) {
                    const ta::Json material = rims[i].value("material", ta::Json::object());
                    partRow("rim:" + str(rims[i], "id"), "Rim " + std::to_string(i + 1), rimSummary(rims[i]), &material, (int)i);
                }
                if (!ctx.readOnly && rims.size() < ta::TITLE_MAX_RIMS && ImGui::Button(ICON_FA_PLUS " Rim", ImVec2(-1.0f, 0.0f))) {
                    std::string error;
                    ctx.state->commit(ta::Json::array({ta::Json{{"op", "add_rim"}, {"layer", "id:" + layers[0]}}}), "Add Rim", &error);
                    if (!error.empty() && ctx.host) ctx.host->reportError(error);
                }
            }
            fieldRow(ctx, refs(layers, "style.knockout"));
            if (beginSection(ctx, "erode", "Erode", objectSummary("erode", memberOr(style, "erode", emptyObject())),
                             memberOr(style, "erode", emptyObject()).value("enabled", false))) {
                ctx.stepPrefix = "Erode ";
                objectRows(ctx, layers, "style.erode", "erode");
                ctx.stepPrefix.clear();
                endSection(ctx);
            }
            endSection(ctx);
        }

        if (type == ta::LayerType::Text) {
            const ta::Json charsCopy = ctx.collecting() ? ta::Json() : first->value("chars", ta::Json::object());
            const ta::Json& chars = ctx.collecting() ? memberOr(*first, "chars", emptyObject()) : charsCopy;
            if (beginSection(ctx, "letters", "Letters", chars.empty() ? std::string() : std::to_string(chars.size()) + " adjusted", false)) {
                if (!ctx.collecting()) {
                    if (chars.empty()) ImGui::TextDisabled("Select characters while editing the text on the canvas to adjust them");
                    for (auto it = chars.begin(); it != chars.end(); ++it) {
                        ImGui::PushID(it.key().c_str());
                        ImGui::BulletText("Character %s: %s", it.key().c_str(), objectSummary("char_adjust", it.value()).c_str());
                        ImGui::SameLine();
                        if (!ctx.readOnly && ImGui::SmallButton("Reset")) {
                            ta::Json rest = chars;
                            rest.erase(it.key());
                            std::string error;
                            ctx.state->commit(editsToOps({{layers[0], "chars", rest}}), "Reset Letter", &error);
                        }
                        ImGui::PopID();
                    }
                    if (!chars.empty() && !ctx.readOnly && ImGui::Button("Reset All")) {
                        std::string error;
                        ctx.state->commit(editsToOps({{layers[0], "chars", ta::Json::object()}}), "Reset All Letters", &error);
                    }
                }
                endSection(ctx);
            }
        }

        if (type != ta::LayerType::Image) {
            const ta::Json warpCopy = ctx.collecting() ? ta::Json() : first->value("warp", ta::Json::object());
            const ta::Json& warp = ctx.collecting() ? memberOr(*first, "warp", emptyObject()) : warpCopy;
            const std::string warpSummary = warp.value("style", std::string("none")) == "none" ? std::string() : enumLabel(warp.value("style", std::string()));
            if (beginSection(ctx, "warp", "Warp & Distort", warpSummary, false)) {
                ctx.stepPrefix = "Warp ";
                objectRows(ctx, layers, "warp", "warp");
                ctx.stepPrefix = "Distort ";
                objectRows(ctx, layers, "distort", "distort");
                ctx.stepPrefix.clear();
                fieldRow(ctx, refs(layers, "weight"));
                endSection(ctx);
            }
        }
    }

    const ta::Json effectsCopy = ctx.collecting() ? ta::Json() : first->value("effects", ta::Json::array());
    const ta::Json& effects = ctx.collecting() ? memberOr(*first, "effects", emptyArray()) : effectsCopy;
    std::string effectsSummary = effects.empty() ? std::string() : std::to_string(effects.size());
    CostBadge stackCost;
    if (!effects.empty() && layers.size() == 1 && !ctx.collecting() && ctx.host && ctx.host->effectCost(layers[0], std::string(), -1, stackCost)) {
        effectsSummary += " \xC2\xB7 " + stackCost.cls;
    }
    if (layers.size() == 1 && beginSection(ctx, "effects", "Effects", effectsSummary, true)) {
        if (!ctx.collecting() && !stackCost.cls.empty()) {
            ImGui::TextDisabled("Stack");
            ImGui::SameLine();
            drawCostBadge(stackCost);
        }
        effectCards(ctx, layers[0], std::string());
        endSection(ctx);
    }
    const ta::Json masksCopy = ctx.collecting() ? ta::Json() : first->value("masks", ta::Json::array());
    const ta::Json& masks = ctx.collecting() ? memberOr(*first, "masks", emptyArray()) : masksCopy;
    if (layers.size() == 1 && beginSection(ctx, "masks", "Masks", masks.empty() ? std::string() : std::to_string(masks.size()), false)) {
        if (!ctx.collecting()) {
            for (size_t i = 0; i < masks.size(); i++) {
                NodeRef node{NodeKind::Mask, layers[0], {}, str(masks[i], "id")};
                ImGui::PushID((int)i);
                if (ImGui::Selectable((enumLabel(str(masks[i], "type")) + " Mask##m").c_str(), ctx.state->isSelected(node)) && ctx.host) ctx.host->selectNode(node);
                ImGui::PopID();
            }
            if (!ctx.readOnly && masks.size() < ta::TITLE_MAX_MASKS) {
                if (ImGui::Button(ICON_FA_PLUS " Add Mask")) ImGui::OpenPopup("##addmask");
                if (ImGui::BeginPopup("##addmask")) {
                    for (const char* maskType : {"shape", "gradient", "layer", "image"}) {
                        if (ImGui::MenuItem(enumLabel(maskType).c_str())) {
                            std::string error;
                            ctx.state->commit(ta::Json::array({ta::Json{{"op", "add_mask"}, {"layer", "id:" + layers[0]}, {"type", maskType}}}),
                                              "Add Mask", &error);
                            if (!error.empty() && ctx.host) ctx.host->reportError(error);
                        }
                    }
                    ImGui::EndPopup();
                }
            }
        }
        endSection(ctx);
    }
    // a Dimension 3D layer opens it: its 3D rows are there (S86 R51)
    const bool layer3D = first->value("dimension", std::string("2d")) == "3d";
    if (ta::layerTypeHasStyle(type) && beginSection(ctx, "dimension", "Dimension", layer3D ? "3D" : "2D", layer3D)) {
        if (!ctx.collecting()) {
            const std::string owner = TitleArtOps::pendingOwner("set_dimension");
            const bool is3d = first->value("dimension", std::string("2d")) == "3d";
            if (!owner.empty()) ImGui::BeginDisabled();
            if (ImGui::RadioButton("2D", !is3d) && is3d) {
                std::string error;
                ctx.state->commit(ta::Json::array({ta::Json{{"op", "set_dimension"}, {"layer", "id:" + layers[0]}, {"dimension", "2d"}}}), "Set Dimension 2D", &error);
                if (!error.empty() && ctx.host) ctx.host->reportError(error);
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("3D", is3d) && !is3d) {
                std::string error;
                ctx.state->commit(ta::Json::array({ta::Json{{"op", "set_dimension"}, {"layer", "id:" + layers[0]}, {"dimension", "3d"}}}), "Set Dimension 3D", &error);
                if (!error.empty() && ctx.host) ctx.host->reportError(error);
            }
            if (!owner.empty()) {
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("(3D titles arrive with %s)", owner.c_str());
            }
        }
        // S86's subsections (Geometry, Bands, Lighting, Glint, ...) of a Dimension 3D layer
        childSections(ctx, SectionDef::Page::Layer, "dimension", layers, {});
        endSection(ctx);
    }
    registeredSections(ctx, SectionDef::Page::Layer, layers, {});
}

void jasperengine::editor::titledesigner::partPage(RowContext& ctx, const std::string& layer, const std::vector<std::string>& parts) {
    JE_PROFILE_SCOPE("TD partPage");
    const ta::TitleArtDoc& doc = ctx.state->shown();
    const ta::Json* object = layerById(doc, layer);
    if (!object || parts.empty()) return;
    const std::string& part = parts[0];
    std::vector<std::string> layers(parts.size(), layer);
    if (const SectionDef* replaced = replacingSection(ctx, SectionDef::Page::Part, "material")) {
        // a Dimension 3D layer's Face, Bevel, Side and Bands (S86 R51)
        ctx.nodeKind = "3d_" + part.substr(0, part.find(':'));
        ctx.stepPrefix.clear();
        ctx.countNoun.clear();
        if (!ctx.collecting()) ImGui::SeparatorText(partLabel(*object, part).c_str());
        replaced->draw(ctx, {layer}, parts);
        if (parts.size() == 1 && beginSection(ctx, "partfx", "Effects", std::string(), true)) {
            effectCards(ctx, layer, part);
            endSection(ctx);
        }
        registeredSections(ctx, SectionDef::Page::Part, {layer}, parts);
        return;
    }
    if (part == "face") {
        ctx.nodeKind = "face";
        ctx.stepPrefix.clear();
        ctx.countNoun.clear();
        materialSections(ctx, {layer}, "style.face", std::string());
    } else if (part.rfind("rim:", 0) == 0) {
        ctx.nodeKind = "rim";
        ctx.countNoun = "Rims";
        // the rims' indexes (several rims of one layer edit together, R12)
        std::vector<std::string> bases;
        // (read in place: used only before any row could edit the document)
        const ta::Json& rims = memberOr(memberOr(*object, "style", emptyObject()), "rims", emptyArray());
        for (const std::string& p : parts) {
            for (size_t i = 0; i < rims.size(); i++) {
                if ("rim:" + str(rims[i], "id") == p) bases.push_back("style.rims[" + std::to_string(i) + "]");
            }
        }
        if (bases.empty()) return;
        const std::string prefix = parts.size() > 1 ? std::string("Rim ") : partLabel(*object, part) + " ";
        ctx.stepPrefix = prefix;
        auto rimRefs = [&](const std::string& member) {
            std::vector<FieldRef> out;
            for (const std::string& b : bases) out.push_back({layer, b + "." + member});
            return out;
        };
        if (!ctx.collecting()) ImGui::SeparatorText(parts.size() > 1 ? (std::to_string(parts.size()) + " Rims").c_str() : partLabel(*object, part).c_str());
        for (const char* key : {"enabled", "width", "join", "fill_counters", "offset"}) fieldRow(ctx, rimRefs(key));
        const ta::Json* plate = ctx.collecting() ? nullptr : fieldValue(doc, {layer, bases[0] + ".plate"});
        const std::string plateSummary = ctx.collecting() ? std::string() : objectSummary("plate", plate ? *plate : ta::Json::object());
        if (beginSection(ctx, "plate", "Plate", plateSummary, false)) {
            ctx.stepPrefix = prefix + "Plate ";
            for (const char* key : {"kind", "close", "corner", "slant"}) fieldRow(ctx, rimRefs(std::string("plate.") + key));
            ctx.stepPrefix = prefix;
            endSection(ctx);
        }
        if (bases.size() == 1) materialSections(ctx, {layer}, bases[0] + ".material", prefix);
    } else {
        // S86's parts (bevel, side, band:) register their sections
        ctx.nodeKind = part.substr(0, part.find(':'));
    }
    if (parts.size() == 1 && beginSection(ctx, "partfx", "Effects", std::string(), true)) {
        effectCards(ctx, layer, part);
        endSection(ctx);
    }
    registeredSections(ctx, SectionDef::Page::Part, {layer}, parts);
}

void jasperengine::editor::titledesigner::effectPage(RowContext& ctx, const NodeRef& node) {
    JE_PROFILE_SCOPE("TD effectPage");
    const ta::TitleArtDoc& doc = ctx.state->shown();
    const ta::Json* object = layerById(doc, node.layer);
    if (!object) return;
    const ta::Json* effect = effectOf(*object, node);
    if (!effect) return;
    const std::string type = str(*effect, "type");
    const ta::EffectDef* def = ta::findEffect(type);
    ctx.nodeKind = "fx:" + type;
    ctx.stepPrefix = std::string(def ? def->label : type.c_str()) + " ";
    ctx.countNoun.clear();
    const std::string base = effectPathOf(*object, node);
    fieldRow(ctx, {{node.layer, base + ".enabled"}}, {.label = "Enabled"});
    if (!def) {
        if (!ctx.collecting()) ImGui::TextDisabled("Unknown effect type '%s': kept as it is, not drawn", type.c_str());
        return;
    }
    if (!ctx.collecting()) {
        ImGui::TextWrapped("%s", def->description);
        if (!def->gizmos.empty()) ImGui::TextDisabled("\xE2\x9C\xA5 Its handles are on the canvas");
    }
    presetChips(ctx, node.layer, base + ".params", def->type, def->params);
    paramRows(ctx, {node.layer}, base + ".params", def->params, def, nullptr);
}

void jasperengine::editor::titledesigner::maskPage(RowContext& ctx, const NodeRef& node) {
    const ta::TitleArtDoc& doc = ctx.state->shown();
    const ta::Json* object = layerById(doc, node.layer);
    if (!object) return;
    const int index = maskIndexOf(*object, node.item);
    if (index < 0) return;
    const ta::Json& mask = (*object)["masks"][(size_t)index];
    const ta::SchemaKind* base = ta::TitleArtSchema::get().kind("mask");
    const ta::SchemaKind* variant = base ? ta::TitleArtSchema::get().variantOf(*base, mask) : nullptr;
    ctx.nodeKind = "mask:" + str(mask, "type");
    ctx.stepPrefix = "Mask ";
    ctx.countNoun.clear();
    const std::string path = "masks[" + std::to_string(index) + "]";
    objectRows(ctx, {node.layer}, path, variant ? variant->name : std::string("mask"));
    if (str(mask, "type") == "shape" && beginSection(ctx, "maskshape", "Shape", enumLabel(mask.value("shape", ta::Json::object()).value("shape", std::string("rect"))), true)) {
        objectRows(ctx, {node.layer}, path + ".shape", "shape_geometry");
        endSection(ctx);
    }
    if (!ctx.collecting() && !ctx.readOnly && ImGui::Button("Remove Mask")) {
        std::string error;
        ctx.state->commit(ta::Json::array({ta::Json{{"op", "remove_mask"}, {"layer", "id:" + node.layer}, {"mask", "mask#" + std::to_string(index)}}}), "Remove Mask",
                          &error);
        if (!error.empty() && ctx.host) ctx.host->reportError(error);
    }
}

void jasperengine::editor::titledesigner::drawSelectionPage(RowContext& ctx) {
    JE_PROFILE_SCOPE("TD drawSelectionPage");
    const NodeRef primary = ctx.state->primary();
    const ta::TitleArtDoc& doc = ctx.state->shown();
    ctx.node = primary;
    const ta::Json* layer = layerById(doc, primary.layer);
    ctx.nodeLabel = layer ? str(*layer, "name") : std::string("Document");
    switch (primary.kind) {
        case NodeKind::Document: documentPage(ctx); break;
        case NodeKind::Layer: {
            std::vector<std::string> layers;
            for (const NodeRef& n : ctx.state->selection()) {
                if (n.kind == NodeKind::Layer) layers.push_back(n.layer);
            }
            layerPage(ctx, layers);
            break;
        }
        case NodeKind::Part: {
            std::vector<std::string> parts;
            for (const NodeRef& n : ctx.state->selection()) {
                if (n.kind == NodeKind::Part && n.layer == primary.layer) parts.push_back(n.part);
            }
            partPage(ctx, primary.layer, parts);
            break;
        }
        case NodeKind::Effect: effectPage(ctx, primary); break;
        case NodeKind::Mask: maskPage(ctx, primary); break;
    }
}

void jasperengine::editor::titledesigner::collectAddOffers(PropertyIndex& index) {
    for (const ta::EffectDef* def : ta::TitleEffectRegistry::get().effects()) {
        AddOffer offer;
        offer.kind = "effect";
        offer.type = def->type;
        offer.label = def->label;
        offer.category = def->category;
        for (const char* alias : def->aliases) offer.aliases.push_back(alias);
        index.addOffer(std::move(offer));
    }
    for (const ta::GeneratorDef* def : ta::TitleEffectRegistry::get().generators()) {
        AddOffer offer;
        offer.kind = "generator";
        offer.type = def->type;
        offer.label = def->label;
        offer.category = def->category;
        for (const char* alias : def->aliases) offer.aliases.push_back(alias);
        index.addOffer(std::move(offer));
    }
}

void jasperengine::editor::titledesigner::collectIndex(TitleDesignerState& state, PropertyHost* host, PropertyIndex& index, bool everyLayer) {
    JE_PROFILE_SCOPE("TD collectIndex");
    index.clear();
    clearCollectAudit();
    const CollectLayerLookup lookup(state.shown());
    for (const CollectUnit& unit : planCollect(state, everyLayer)) collectUnit(state, host, index, unit);
}

std::vector<CollectUnit> jasperengine::editor::titledesigner::planCollect(const TitleDesignerState& state, bool everyLayer) {
    std::vector<CollectUnit> units;
    const ta::TitleArtDoc& doc = state.shown();
    std::vector<std::string> layers;
    if (everyLayer) {
        ta::forEachLayer(doc.root, [&](const ta::Json& l, const ta::Json*, int) {
            layers.push_back(str(l, "id"));
            return true;
        });
    } else {
        layers = state.selectedLayers();
    }
    auto array = [](const ta::Json& object, const char* key) -> const ta::Json* {
        auto it = object.find(key);
        return it != object.end() && it->is_array() ? &*it : nullptr;
    };
    for (const std::string& id : layers) {
        const ta::Json* layer = layerById(doc, id);
        if (!layer) continue;
        // the layer page, the costliest, a top-level section per unit
        for (int slice = 0; slice < LAYER_PAGE_SLICES; slice++) units.push_back({CollectUnit::Kind::Layer, {NodeKind::Layer, id, {}, {}}, slice});
        if (ta::layerTypeHasStyle(ta::layerTypeOf(*layer)) && layer->value("dimension", std::string("2d")) == "3d") {
            // a Dimension 3D layer's parts (S86): Face, Bevel, Side, Band N
            for (const char* part : {"face", "bevel", "side"}) units.push_back({CollectUnit::Kind::Part, {NodeKind::Part, id, part, {}}});
            auto style3d = layer->find("style_3d");
            if (style3d != layer->end() && style3d->is_object()) {
                if (const ta::Json* bands = array(*style3d, "bands")) {
                    for (const ta::Json& band : *bands) units.push_back({CollectUnit::Kind::Part, {NodeKind::Part, id, "band:" + str(band, "id"), {}}});
                }
            }
        } else if (ta::layerTypeHasStyle(ta::layerTypeOf(*layer))) {
            units.push_back({CollectUnit::Kind::Part, {NodeKind::Part, id, "face", {}}});
            auto style = layer->find("style");
            if (style != layer->end() && style->is_object()) {
                if (const ta::Json* rims = array(*style, "rims")) {
                    for (const ta::Json& rim : *rims) units.push_back({CollectUnit::Kind::Part, {NodeKind::Part, id, "rim:" + str(rim, "id"), {}}});
                }
            }
        }
        if (const ta::Json* effects = array(*layer, "effects")) {
            for (const ta::Json& effect : *effects) units.push_back({CollectUnit::Kind::Effect, {NodeKind::Effect, id, {}, str(effect, "id")}});
        }
        auto parts = layer->find("part_effects");
        if (parts != layer->end() && parts->is_object()) {
            for (auto it = parts->begin(); it != parts->end(); ++it) {
                if (!it.value().is_array()) continue;
                for (const ta::Json& effect : it.value()) units.push_back({CollectUnit::Kind::Effect, {NodeKind::Effect, id, it.key(), str(effect, "id")}});
            }
        }
        if (const ta::Json* masks = array(*layer, "masks")) {
            for (const ta::Json& mask : *masks) units.push_back({CollectUnit::Kind::Mask, {NodeKind::Mask, id, {}, str(mask, "id")}});
        }
    }
    units.push_back({CollectUnit::Kind::Document, {}});
    units.push_back({CollectUnit::Kind::Offers, {}});
    return units;
}

void jasperengine::editor::titledesigner::collectUnit(TitleDesignerState& state, PropertyHost* host, PropertyIndex& index, const CollectUnit& unit) {
    JE_PROFILE_SCOPE("TD collectUnit");
    const ta::TitleArtDoc& doc = state.shown();
    RowContext ctx;
    ctx.mode = RowMode::Collect;
    ctx.host = host;
    ctx.state = &state;
    ctx.sink = &index;
    ctx.allOpen = true;
    ctx.readOnly = !state.editable();
    ctx.node = unit.node;
    if (unit.kind == CollectUnit::Kind::Document) {
        ctx.node = {};
        ctx.nodeLabel = "Document";
        documentPage(ctx);
        return;
    }
    if (unit.kind == CollectUnit::Kind::Offers) {
        collectAddOffers(index);
        return;
    }
    const ta::Json* layer = layerById(doc, unit.node.layer);
    if (!layer) return;
    ctx.nodeLabel = str(*layer, "name");
    switch (unit.kind) {
        case CollectUnit::Kind::Layer:
            ctx.onlySection = unit.slice;
            ctx.sliceTakesRest = unit.slice == LAYER_PAGE_SLICES - 1;
            layerPage(ctx, {unit.node.layer});
            break;
        case CollectUnit::Kind::Part:
            ctx.sectionChain = {partLabel(*layer, unit.node.part)};
            partPage(ctx, unit.node.layer, {unit.node.part});
            break;
        case CollectUnit::Kind::Effect: {
            const ta::Json* effect = effectOf(*layer, unit.node);
            if (!effect) return;
            const ta::EffectDef* def = ta::findEffect(str(*effect, "type"));
            if (!unit.node.part.empty()) ctx.sectionChain.push_back(partLabel(*layer, unit.node.part));
            ctx.sectionChain.push_back(def ? def->label : str(*effect, "type"));
            effectPage(ctx, ctx.node);
            break;
        }
        case CollectUnit::Kind::Mask: {
            const ta::Json* mask = nullptr;
            auto masks = layer->find("masks");
            if (masks != layer->end() && masks->is_array()) {
                for (const ta::Json& m : *masks) {
                    if (str(m, "id") == unit.node.item) mask = &m;
                }
            }
            if (!mask) return;
            ctx.sectionChain = {enumLabel(str(*mask, "type")) + " Mask"};
            maskPage(ctx, ctx.node);
            break;
        }
        default:
            break;
    }
}
