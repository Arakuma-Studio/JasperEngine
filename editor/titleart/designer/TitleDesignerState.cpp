// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleDesignerState.h"
#include "titleart/localisation/TitleLanguageEditing.h"
#include "titleart/TitleTranslations.h"

#include "Project.h"
#include "titleart/TitleArtIO.h"

#include <algorithm>
#include <map>
#include <random>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    bool fail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    uint64_t newSeed() {
        static std::mt19937_64 random(std::random_device{}());
        uint64_t seed = 0;
        while (seed == 0) seed = random();
        return seed;
    }

    std::string str(const ta::Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // the siblings array holding a layer and its index there
    bool findSiblings(const ta::Json& list, const std::string& id, const ta::Json*& siblings, size_t& index, std::string& parent,
                      const std::string& parentId) {
        if (!list.is_array()) return false;
        for (size_t i = 0; i < list.size(); i++) {
            const ta::Json& layer = list[i];
            if (str(layer, "id") == id) {
                siblings = &list;
                index = i;
                parent = parentId;
                return true;
            }
            auto children = layer.find("children");
            if (children != layer.end() && findSiblings(*children, id, siblings, index, parent, str(layer, "id"))) return true;
        }
        return false;
    }

    // "Name" or "Name#N" (the Nth same-named sibling after the first)
    std::string segmentOf(const ta::Json& siblings, size_t index) {
        const std::string name = str(siblings[index], "name");
        int before = 0;
        for (size_t i = 0; i < index; i++) {
            if (str(siblings[i], "name") == name) before++;
        }
        return before == 0 ? name : name + "#" + std::to_string(before);
    }

    std::vector<std::string> splitPath(const std::string& path) {
        std::vector<std::string> out;
        std::string current;
        for (char c : path) {
            if (c == '/') {
                out.push_back(current);
                current.clear();
            } else {
                current += c;
            }
        }
        out.push_back(current);
        return out;
    }

    bool parseIndexed(const std::string& text, const std::string& prefix, int& index) {
        if (text.rfind(prefix, 0) != 0) return false;
        const std::string digits = text.substr(prefix.size());
        if (digits.empty() || digits.size() > 6 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
        index = std::stoi(digits);
        return true;
    }

}

const char* jasperengine::editor::titledesigner::previewKindName(PreviewItem::Kind kind) {
    switch (kind) {
        case PreviewItem::Kind::Look: return "look";
        case PreviewItem::Kind::ColorScheme: return "color_scheme";
        case PreviewItem::Kind::Template: return "template";
        case PreviewItem::Kind::Effect: return "effect";
        case PreviewItem::Kind::Generator: return "generator";
        case PreviewItem::Kind::Layer: return "layer";
        case PreviewItem::Kind::Font: return "font";
        case PreviewItem::Kind::HistoryVersion: return "history_version";
    }
    return "";
}

// ---- paths ----

std::string jasperengine::editor::titledesigner::layerPathOf(const TitleArtDoc& doc, const std::string& layerId) {
    std::vector<std::string> segments;
    std::string id = layerId;
    while (!id.empty()) {
        const ta::Json* siblings = nullptr;
        size_t index = 0;
        std::string parent;
        if (!findSiblings(doc.layers(), id, siblings, index, parent, "")) return {};
        segments.push_back(segmentOf(*siblings, index));
        id = parent;
    }
    std::string out;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        if (!out.empty()) out += '/';
        out += *it;
    }
    return out;
}

const ta::Json* jasperengine::editor::titledesigner::layerById(const TitleArtDoc& doc, const std::string& layerId) {
    if (layerId.empty()) return nullptr;
    return ta::findLayer(doc.root, layerId);
}

std::string jasperengine::editor::titledesigner::parentLayerOf(const TitleArtDoc& doc, const std::string& layerId) {
    const ta::Json* siblings = nullptr;
    size_t index = 0;
    std::string parent;
    if (!findSiblings(doc.layers(), layerId, siblings, index, parent, "")) return {};
    return parent;
}

std::string jasperengine::editor::titledesigner::rimPartKey(const ta::Json& layer, size_t index) {
    const ta::Json* rims = nullptr;
    auto style = layer.find("style");
    if (style != layer.end()) {
        auto r = style->find("rims");
        if (r != style->end() && r->is_array()) rims = &*r;
    }
    if (!rims || index >= rims->size()) return {};
    return "rim:" + str((*rims)[index], "id");
}

std::string jasperengine::editor::titledesigner::partLabel(const ta::Json& layer, const std::string& partKey) {
    if (partKey == "face") return "Face";
    if (partKey == "bevel") return "Bevel";
    if (partKey == "side") return "Side";
    if (partKey.rfind("rim:", 0) == 0) {
        const std::string id = partKey.substr(4);
        auto style = layer.find("style");
        if (style != layer.end()) {
            auto rims = style->find("rims");
            if (rims != style->end() && rims->is_array()) {
                for (size_t i = 0; i < rims->size(); i++) {
                    if (str((*rims)[i], "id") == id) return "Rim " + std::to_string(i + 1);
                }
            }
        }
        return "Rim";
    }
    if (partKey.rfind("band:", 0) == 0) return "Band";
    return partKey;
}

const ta::Json* jasperengine::editor::titledesigner::effectOf(const ta::Json& layer, const NodeRef& node) {
    const ta::Json* list = nullptr;
    if (node.part.empty()) {
        auto it = layer.find("effects");
        if (it != layer.end()) list = &*it;
    } else {
        auto parts = layer.find("part_effects");
        if (parts != layer.end() && parts->is_object()) {
            auto it = parts->find(node.part);
            if (it != parts->end()) list = &*it;
        }
    }
    if (!list || !list->is_array()) return nullptr;
    for (const ta::Json& effect : *list) {
        if (str(effect, "id") == node.item) return &effect;
    }
    return nullptr;
}

std::string jasperengine::editor::titledesigner::effectPathOf(const ta::Json& layer, const NodeRef& node) {
    const ta::Json* list = nullptr;
    std::string prefix;
    if (node.part.empty()) {
        auto it = layer.find("effects");
        if (it != layer.end()) list = &*it;
        prefix = "effects";
    } else {
        auto parts = layer.find("part_effects");
        if (parts != layer.end() && parts->is_object()) {
            auto it = parts->find(node.part);
            if (it != parts->end()) list = &*it;
        }
        prefix = "part_effects." + node.part;
    }
    if (!list || !list->is_array()) return {};
    for (size_t i = 0; i < list->size(); i++) {
        if (str((*list)[i], "id") == node.item) return prefix + "[" + std::to_string(i) + "]";
    }
    return {};
}

int jasperengine::editor::titledesigner::maskIndexOf(const ta::Json& layer, const std::string& maskId) {
    auto masks = layer.find("masks");
    if (masks == layer.end() || !masks->is_array()) return -1;
    for (size_t i = 0; i < masks->size(); i++) {
        if (str((*masks)[i], "id") == maskId) return (int)i;
    }
    return -1;
}

std::string jasperengine::editor::titledesigner::nodePath(const TitleArtDoc& doc, const NodeRef& node) {
    if (node.kind == NodeKind::Document) return {};
    std::string out = layerPathOf(doc, node.layer);
    const ta::Json* layer = layerById(doc, node.layer);
    if (out.empty() || !layer) return {};
    auto partSegment = [&](const std::string& part) -> std::string {
        if (part.rfind("rim:", 0) == 0) {
            auto style = layer->find("style");
            if (style != layer->end()) {
                auto rims = style->find("rims");
                if (rims != style->end() && rims->is_array()) {
                    for (size_t i = 0; i < rims->size(); i++) {
                        if ("rim:" + str((*rims)[i], "id") == part) return "rim#" + std::to_string(i);
                    }
                }
            }
        }
        return part;
    };
    switch (node.kind) {
        case NodeKind::Layer: return out;
        case NodeKind::Part: return out + "/" + partSegment(node.part);
        case NodeKind::Effect: {
            const ta::Json* effect = effectOf(*layer, node);
            if (!effect) return out;
            const std::string type = str(*effect, "type");
            // fx:<type>#N counts the same types before it in its stack
            const ta::Json* list = nullptr;
            if (node.part.empty()) list = &layer->at("effects");
            else list = &layer->at("part_effects").at(node.part);
            int before = 0;
            for (const ta::Json& e : *list) {
                if (&e == effect) break;
                if (str(e, "type") == type) before++;
            }
            std::string fx = "fx:" + type + (before > 0 ? "#" + std::to_string(before) : std::string());
            return node.part.empty() ? out + "/" + fx : out + "/" + partSegment(node.part) + "/" + fx;
        }
        case NodeKind::Mask: {
            const int index = maskIndexOf(*layer, node.item);
            return index < 0 ? out : out + "/mask#" + std::to_string(index);
        }
        case NodeKind::Document: break;
    }
    return out;
}

bool jasperengine::editor::titledesigner::nodeFromPath(const TitleArtDoc& doc, const std::string& path, NodeRef& out, std::string* error) {
    out = {};
    if (path.empty()) return true;
    if (path.rfind("id:", 0) == 0 && path.find('/') == std::string::npos) {
        if (!layerById(doc, path.substr(3))) return fail(error, "no layer has the id '" + path.substr(3) + "'");
        out.kind = NodeKind::Layer;
        out.layer = path.substr(3);
        return true;
    }
    const std::vector<std::string> segments = splitPath(path);
    const ta::Json* list = &doc.layers();
    const ta::Json* layer = nullptr;
    size_t used = 0;
    // layer segments first: names (#N), a bare name anywhere when it is unique
    for (; used < segments.size(); used++) {
        const std::string& segment = segments[used];
        if (segment.rfind("fx:", 0) == 0 || segment.rfind("rim#", 0) == 0 || segment.rfind("rim:", 0) == 0 || segment.rfind("mask#", 0) == 0 ||
            segment == "face" || segment == "bevel" || segment == "side" || segment.rfind("band", 0) == 0) {
            if (layer) break;
        }
        if (!list || !list->is_array()) break;
        std::string name = segment;
        int nth = 0;
        const size_t hash = segment.rfind('#');
        if (hash != std::string::npos) {
            int n = 0;
            if (parseIndexed(segment.substr(hash), "#", n)) {
                name = segment.substr(0, hash);
                nth = n;
            }
        }
        const ta::Json* found = nullptr;
        int seen = 0;
        std::vector<std::string> names;
        for (const ta::Json& candidate : *list) {
            names.push_back(str(candidate, "name"));
            if (str(candidate, "name") == name) {
                if (seen == nth) {
                    found = &candidate;
                    break;
                }
                seen++;
            }
        }
        if (!found && used == 0 && segments.size() >= 1) {
            // a bare name anywhere, when unique
            std::vector<const ta::Json*> matches;
            ta::forEachLayer(doc.root, [&](const ta::Json& l, const ta::Json*, int) {
                if (str(l, "name") == name) matches.push_back(&l);
                return true;
            });
            if (matches.size() == 1) found = matches[0];
            else if (matches.size() > 1) return fail(error, "'" + name + "' names " + std::to_string(matches.size()) + " layers: use its path");
        }
        if (!found) {
            std::string valid;
            for (const std::string& n : names) valid += (valid.empty() ? "" : ", ") + n;
            return fail(error, "no layer '" + segment + "' (valid here: " + (valid.empty() ? "none" : valid) + ")");
        }
        layer = found;
        auto children = found->find("children");
        list = children != found->end() ? &*children : nullptr;
    }
    if (!layer) return fail(error, "'" + path + "' names no layer");
    out.kind = NodeKind::Layer;
    out.layer = str(*layer, "id");
    if (used == segments.size()) return true;

    std::string part;
    const std::string& segment = segments[used];
    int index = 0;
    if (parseIndexed(segment, "rim#", index)) {
        part = rimPartKey(*layer, (size_t)index);
        if (part.empty()) return fail(error, "'" + segment + "': the layer has no rim " + std::to_string(index));
    } else if (segment.rfind("rim:", 0) == 0 || segment == "face" || segment == "bevel" || segment == "side" || segment.rfind("band:", 0) == 0) {
        part = segment;
    } else if (parseIndexed(segment, "mask#", index)) {
        auto masks = layer->find("masks");
        if (masks == layer->end() || !masks->is_array() || (size_t)index >= masks->size()) {
            return fail(error, "'" + segment + "': the layer has no mask " + std::to_string(index));
        }
        out.kind = NodeKind::Mask;
        out.item = str((*masks)[index], "id");
        return used + 1 == segments.size() || fail(error, "nothing follows a mask in a path");
    }
    if (!part.empty()) {
        out.kind = NodeKind::Part;
        out.part = part;
        used++;
        if (used == segments.size()) return true;
    }
    const std::string& fx = segments[used];
    if (fx.rfind("fx:", 0) != 0) return fail(error, "'" + fx + "' is not a part (face, rim#N), an effect (fx:<type>) or a mask (mask#N)");
    std::string type = fx.substr(3);
    int nth = 0;
    const size_t hash = type.rfind('#');
    if (hash != std::string::npos && parseIndexed(type.substr(hash), "#", nth)) type = type.substr(0, hash);
    const ta::Json* list2 = nullptr;
    if (part.empty()) {
        auto it = layer->find("effects");
        if (it != layer->end()) list2 = &*it;
    } else {
        auto parts = layer->find("part_effects");
        if (parts != layer->end()) {
            auto it = parts->find(part);
            if (it != parts->end()) list2 = &*it;
        }
    }
    int seen = 0;
    std::string valid;
    if (list2 && list2->is_array()) {
        for (const ta::Json& effect : *list2) {
            valid += (valid.empty() ? "" : ", ") + std::string("fx:") + str(effect, "type");
            if (str(effect, "type") == type) {
                if (seen == nth) {
                    out.kind = NodeKind::Effect;
                    out.part = part;
                    out.item = str(effect, "id");
                    return used + 1 == segments.size() || fail(error, "nothing follows an effect in a path");
                }
                seen++;
            }
        }
    }
    return fail(error, "no effect '" + fx + "' (valid here: " + (valid.empty() ? "none" : valid) + ")");
}

// ---- state ----

TitleDesignerState::TitleDesignerState(Project* project) : owner(project) {
}

TitleDesignerState::~TitleDesignerState() {
    close();
}

bool TitleDesignerState::open(const fs::path& file, std::string* error) {
    std::string why;
    std::shared_ptr<TitleArtDocument> opened = TitleArtDocument::open(owner, file.generic_string(), &why);
    if (!opened) return fail(error, why);
    if (opened == doc) return true;
    close();
    doc = opened;
    languageCode.clear();
    listener = doc->addListener([this](TitleArtChangeOrigin origin) { documentChanged(origin); });
    selected.clear();
    rebuild();
    return true;
}

void TitleDesignerState::close() {
    if (doc && listener) doc->removeListener(listener);
    listener = 0;
    doc.reset();
    gesture = false;
    staged = ta::Json::array();
    previewItem.reset();
    previewDoc.reset();
    selected.clear();
    external.reset();
    workingDoc = emptyDoc;
    changes++;
}

bool TitleDesignerState::editable() const {
    return doc && doc->editable() && doc->loadError().empty();
}

std::string TitleDesignerState::readOnlyReason() const {
    if (!doc) return "No Title Art is open";
    if (!doc->editable()) return doc->versionError();
    if (!doc->loadError().empty()) return "The file does not parse: " + doc->loadError();
    return {};
}

const TitleArtDoc& TitleDesignerState::committed() const {
    return doc ? doc->doc() : emptyDoc;
}

std::shared_ptr<const TitleArtDoc> TitleDesignerState::shownSnapshot() const {
    if (!snapshot || snapshotChange != changes) {
        snapshot = std::make_shared<const TitleArtDoc>(shown());
        snapshotChange = changes;
    }
    return snapshot;
}

ta::Json TitleDesignerState::routed(const ta::Json& ops) const {
    const ta::Json routedOps = editRouter ? editRouter(ops) : ops;
    // S87 R52: in a translation language, overridable members go to its Translation
    return languageCode.empty() ? routedOps : routeTitleLanguageOps(committed(), languageCode, routedOps);
}

void TitleDesignerState::setLanguage(const std::string& code) {
    if (code == languageCode) return;
    languageCode = ta::isSourceLanguage(committed().root, code) ? std::string() : code;
    rebuild();
}

void TitleDesignerState::rebuild() {
    workingDoc = committed();
    if (gesture && !staged.empty() && doc) {
        TitleOpsResult applied = TitleArtOps::apply(workingDoc, staged);
        if (applied.ok) workingDoc = std::move(applied.doc);
    }
    rebuildPreview();
    pruneSelection();
    changes++;
}

void TitleDesignerState::rebuildPreview() {
    previewDoc.reset();
    if (!previewItem) return;
    if (previewItem->kind == PreviewItem::Kind::HistoryVersion) {
        ta::TitleArtParseResult parsed = ta::parseTitleArt(previewItem->text);
        if (parsed.ok) previewDoc = std::make_shared<TitleArtDoc>(std::move(parsed.doc));
        return;
    }
    TitleOpsOptions options;
    options.idSeed = previewSeed;
    TitleOpsResult applied = TitleArtOps::apply(workingDoc, routed(previewItem->ops), options);
    if (applied.ok) previewDoc = std::make_shared<TitleArtDoc>(std::move(applied.doc));
}

void TitleDesignerState::beginGesture(const std::string& stepName, const std::string& mergeKey) {
    if (gesture) cancelGesture();
    gesture = true;
    gestureStep = stepName;
    gestureMergeKey = mergeKey;
    staged = ta::Json::array();
}

bool TitleDesignerState::stage(const ta::Json& ops, std::string* error) {
    if (!editable()) return fail(error, readOnlyReason());
    if (!gesture) beginGesture("Edit Title Art");
    const ta::Json batch = routed(ops.is_array() ? ops : ta::Json::array({ops}));
    TitleOpsResult applied = TitleArtOps::apply(committed(), batch);
    if (!applied.ok) return fail(error, applied.error());
    staged = batch;
    workingDoc = std::move(applied.doc);
    rebuildPreview();
    changes++;
    if (commitPerStage) {
        // the red control of the one-step-per-gesture tests: every stage is a step
        committing = true;
        std::string why;
        TitleArtEditOptions options;
        options.undoName = gestureStep;
        doc->edit(staged, options);
        committing = false;
    }
    return true;
}

bool TitleDesignerState::endGesture(std::string* error) {
    if (!gesture) return true;
    gesture = false;
    if (staged.empty() || commitPerStage) {
        staged = ta::Json::array();
        rebuild();
        return true;
    }
    const ta::Json batch = staged;
    staged = ta::Json::array();
    TitleArtEditOptions options;
    options.undoName = gestureStep;
    options.mergeKey = gestureMergeKey;
    committing = true;
    result = doc->edit(batch, options);
    committing = false;
    rebuild();
    if (!result.ok) return fail(error, result.error());
    return true;
}

void TitleDesignerState::cancelGesture() {
    if (!gesture) return;
    gesture = false;
    staged = ta::Json::array();
    rebuild();
}

bool TitleDesignerState::commit(const ta::Json& ops, const std::string& stepName, std::string* error, const std::string& mergeKey, uint64_t idSeed) {
    if (!editable()) return fail(error, readOnlyReason());
    if (gesture) cancelGesture();
    TitleArtEditOptions options;
    options.undoName = stepName;
    options.mergeKey = mergeKey;
    options.ops.idSeed = idSeed;
    committing = true;
    result = doc->edit(routed(ops.is_array() ? ops : ta::Json::array({ops})), options);
    committing = false;
    rebuild();
    if (!result.ok) return fail(error, result.error());
    return true;
}

bool TitleDesignerState::commitText(const std::string& text, const std::string& stepName, std::string* error) {
    if (!editable()) return fail(error, readOnlyReason());
    if (gesture) cancelGesture();
    committing = true;
    std::string why;
    const bool ok = doc->commitText(text, stepName, &why);
    committing = false;
    rebuild();
    if (!ok) return fail(error, why);
    return true;
}

bool TitleDesignerState::setPreview(const PreviewItem& item, std::string* error) {
    if (!doc) return fail(error, "No Title Art is open");
    if (previewItem && previewItem->kind == item.kind && previewItem->id == item.id && previewItem->ops == item.ops && previewItem->text == item.text) {
        return previewDoc != nullptr || fail(error, "the preview does not apply");
    }
    previewItem = item;
    previewSeed = newSeed();
    rebuildPreview();
    changes++;
    if (!previewDoc) {
        // a refused item shows nothing rather than a stale picture
        std::string why = "the preview does not apply";
        if (item.kind != PreviewItem::Kind::HistoryVersion) {
            TitleOpsResult applied = TitleArtOps::apply(workingDoc, routed(item.ops));
            if (!applied.ok) why = applied.error();
        }
        previewItem.reset();
        return fail(error, why);
    }
    return true;
}

void TitleDesignerState::clearPreview() {
    if (!previewItem) return;
    previewItem.reset();
    previewDoc.reset();
    changes++;
}

bool TitleDesignerState::commitPreview(std::string* error) {
    if (!previewItem) return fail(error, "nothing is previewed");
    const PreviewItem item = *previewItem;
    const uint64_t seed = previewSeed;
    previewItem.reset();
    previewDoc.reset();
    if (item.kind == PreviewItem::Kind::HistoryVersion) return commitText(item.text, item.stepName, error);
    return commit(item.ops, item.stepName, error, {}, seed);
}

std::string TitleDesignerState::newId(size_t opIndex, const char* key) const {
    const auto it = result.newIds.find(std::to_string(opIndex));
    if (it == result.newIds.end() || !it->is_object()) return {};
    const auto id = it->find(key);
    return id != it->end() && id->is_string() ? id->get<std::string>() : std::string();
}

std::vector<std::string> TitleDesignerState::newIds(const char* key) const {
    std::vector<std::pair<size_t, std::string>> found;
    for (auto it = result.newIds.begin(); it != result.newIds.end(); ++it) {
        if (!it->is_object()) continue;
        const auto id = it->find(key);
        if (id != it->end() && id->is_string()) found.push_back({(size_t)std::stoul(it.key()), id->get<std::string>()});
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

void TitleDesignerState::documentChanged(TitleArtChangeOrigin origin) {
    if (committing) return;
    ExternalChange change;
    change.origin = origin;
    change.revision = doc ? doc->revision() : 0;
    if (gesture) {
        gesture = false;
        staged = ta::Json::array();
        change.cancelledGesture = true;
    }
    external = change;
    rebuild();
}

std::optional<ExternalChange> TitleDesignerState::takeExternalChange() {
    std::optional<ExternalChange> out = external;
    external.reset();
    return out;
}

// ---- selection ----

NodeRef TitleDesignerState::primary() const {
    return selected.empty() ? NodeRef{} : selected.back();
}

void TitleDesignerState::select(std::vector<NodeRef> nodes) {
    std::vector<NodeRef> unique;
    for (NodeRef& node : nodes) {
        if (node.kind == NodeKind::Document) continue;
        if (std::find(unique.begin(), unique.end(), node) == unique.end()) unique.push_back(std::move(node));
    }
    selected = std::move(unique);
    pruneSelection();
}

void TitleDesignerState::toggle(const NodeRef& node) {
    auto it = std::find(selected.begin(), selected.end(), node);
    if (it != selected.end()) selected.erase(it);
    else if (node.kind != NodeKind::Document) selected.push_back(node);
}

bool TitleDesignerState::isSelected(const NodeRef& node) const {
    return std::find(selected.begin(), selected.end(), node) != selected.end();
}

std::vector<std::string> TitleDesignerState::selectedLayers() const {
    std::vector<std::string> out;
    for (const NodeRef& node : selected) {
        if (!node.layer.empty() && std::find(out.begin(), out.end(), node.layer) == out.end()) out.push_back(node.layer);
    }
    return out;
}

void TitleDesignerState::selectParent() {
    NodeRef node = primary();
    switch (node.kind) {
        case NodeKind::Document: return;
        case NodeKind::Effect:
            if (!node.part.empty()) {
                node.kind = NodeKind::Part;
                node.item.clear();
            } else {
                node.kind = NodeKind::Layer;
                node.item.clear();
            }
            break;
        case NodeKind::Mask:
            node.kind = NodeKind::Layer;
            node.item.clear();
            break;
        case NodeKind::Part:
            node.kind = NodeKind::Layer;
            node.part.clear();
            break;
        case NodeKind::Layer: {
            const std::string parent = parentLayerOf(shown(), node.layer);
            if (parent.empty()) {
                selected.clear();
                return;
            }
            node.layer = parent;
            break;
        }
    }
    selected = {node};
}

void TitleDesignerState::pruneSelection() {
    const TitleArtDoc& current = shown();
    std::vector<NodeRef> kept;
    for (const NodeRef& node : selected) {
        const ta::Json* layer = layerById(current, node.layer);
        if (!layer) continue;
        bool exists = true;
        switch (node.kind) {
            case NodeKind::Effect: exists = effectOf(*layer, node) != nullptr; break;
            case NodeKind::Mask: exists = maskIndexOf(*layer, node.item) >= 0; break;
            case NodeKind::Part:
                exists = node.part == "face" || node.part == "bevel" || node.part == "side" || node.part.rfind("band:", 0) == 0 ||
                         partLabel(*layer, node.part) != "Rim";
                break;
            default: break;
        }
        if (exists) kept.push_back(node);
    }
    selected = std::move(kept);
}
