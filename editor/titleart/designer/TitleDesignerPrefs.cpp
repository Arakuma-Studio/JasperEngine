// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleDesignerPrefs.h"

#include "AppSettings.h"
#include "DocumentFile.h"
#include "Out.h"
#include "Workspace.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;
namespace fs = std::filesystem;

namespace {

    template<class T>
    T read(const YAML::Node& node, const char* key, T fallback) {
        if (!node || !node.IsMap() || !node[key]) return fallback;
        try {
            return node[key].as<T>();
        } catch (const std::exception&) {
            return fallback;
        }
    }

    std::map<std::string, bool> readFlags(const YAML::Node& node) {
        std::map<std::string, bool> out;
        if (!node || !node.IsMap()) return out;
        for (auto it = node.begin(); it != node.end(); ++it) {
            try {
                out[it->first.as<std::string>()] = it->second.as<bool>();
            } catch (const std::exception&) {
            }
        }
        return out;
    }

    YAML::Node writeFlags(const std::map<std::string, bool>& flags) {
        YAML::Node node(YAML::NodeType::Map);
        for (const auto& [key, value] : flags) node[key] = value;
        return node;
    }

}

const char* jasperengine::editor::titledesigner::qualityName(Quality quality) {
    switch (quality) {
        case Quality::Auto: return "auto";
        case Quality::Draft: return "draft";
        case Quality::Full: return "full";
    }
    return "auto";
}

bool jasperengine::editor::titledesigner::qualityFromName(const std::string& name, Quality& out) {
    for (Quality q : {Quality::Auto, Quality::Draft, Quality::Full}) {
        if (name == qualityName(q)) {
            out = q;
            return true;
        }
    }
    return false;
}

// ---- settings ----

YAML::Node TitleDesignerSettings::encode() const {
    YAML::Node node;
    node["layers_width"] = layersWidth;
    node["properties_width"] = propertiesWidth;
    node["library_height"] = libraryHeight;
    node["layers_collapsed"] = layersCollapsed;
    node["properties_collapsed"] = propertiesCollapsed;
    node["library_collapsed"] = libraryCollapsed;
    if (!sections.empty()) node["sections"] = writeFlags(sections);
    if (budgetSet) node["preview_budget_ms"] = previewBudgetMs;
    if (qualitySet) node["quality"] = qualityName(quality);
    node["stats"] = stats;
    node["snap_move"] = snapMove;
    node["snap_rotate"] = snapRotate;
    node["snap_scale"] = snapScale;
    if (softwareNoticeShown) node["software_notice_shown"] = true;
    node["open"] = windowOpen;
    return node;
}

TitleDesignerSettings TitleDesignerSettings::decode(const YAML::Node& node) {
    TitleDesignerSettings s;
    s.layersWidth = std::clamp(read(node, "layers_width", s.layersWidth), 140.0f, 900.0f);
    s.propertiesWidth = std::clamp(read(node, "properties_width", s.propertiesWidth), 200.0f, 1000.0f);
    s.libraryHeight = std::clamp(read(node, "library_height", s.libraryHeight), 80.0f, 900.0f);
    s.layersCollapsed = read(node, "layers_collapsed", false);
    s.propertiesCollapsed = read(node, "properties_collapsed", false);
    s.libraryCollapsed = read(node, "library_collapsed", false);
    if (node && node.IsMap()) s.sections = readFlags(node["sections"]);
    if (node && node.IsMap() && node["preview_budget_ms"]) {
        s.previewBudgetMs = std::clamp(read(node, "preview_budget_ms", 4.0f), 1.0f, 16.0f);
        s.budgetSet = true;
    }
    if (node && node.IsMap() && node["quality"]) s.qualitySet = qualityFromName(read<std::string>(node, "quality", "auto"), s.quality);
    s.stats = read(node, "stats", false);
    s.snapMove = std::clamp(read(node, "snap_move", 10.0f), 0.1f, 1000.0f);
    s.snapRotate = std::clamp(read(node, "snap_rotate", 15.0f), 0.1f, 180.0f);
    s.snapScale = std::clamp(read(node, "snap_scale", 0.1f), 0.001f, 10.0f);
    s.softwareNoticeShown = read(node, "software_notice_shown", false);
    s.windowOpen = read(node, "open", false);
    return s;
}

TitleDesignerSettings TitleDesignerSettings::load() {
    return decode(AppSettings::getTitleDesignerSettings());
}

void TitleDesignerSettings::save() const {
    AppSettings::setTitleDesignerSettings(encode());
}

// ---- document view ----

YAML::Node DocumentView::encode() const {
    YAML::Node node;
    if (!twirls.empty()) node["twirls"] = writeFlags(twirls);
    YAML::Node b;
    b["kind"] = backdrop.kind;
    b["color"] = backdrop.color;
    if (!backdrop.image.empty()) b["image"] = backdrop.image;
    if (!backdrop.scene.empty()) b["scene"] = backdrop.scene;
    node["backdrop"] = b;
    if (!device.profile.empty()) {
        YAML::Node d;
        d["profile"] = device.profile;
        d["orientation"] = device.orientation;
        d["display_height"] = device.displayHeight;
        d["display_height_px"] = device.displayHeightPx;
        d["anchor"] = device.anchor;
        node["device"] = d;
    }
    if (!guides.empty()) {
        YAML::Node list(YAML::NodeType::Sequence);
        for (const Guide& g : guides) {
            YAML::Node entry;
            entry["axis"] = g.vertical ? "x" : "y";
            entry["at"] = g.position;
            list.push_back(entry);
        }
        node["guides"] = list;
    }
    node["rulers"] = rulers;
    node["show_guides"] = showGuides;
    node["safe_areas"] = safeAreas;
    node["pixel_grid"] = pixelGrid;
    node["effect_reach"] = effectReach;
    node["snap"] = snap;
    node["fit"] = fit;
    node["zoom"] = zoom;
    node["pan_x"] = panX;
    node["pan_y"] = panY;
    node["preview_as"] = previewAs;
    node["library_tab"] = libraryTab;
    if (!libraryFamily.empty()) node["library_family"] = libraryFamily;
    if (!libraryQuery.empty()) node["library_query"] = libraryQuery;
    node["library_dimension"] = libraryDimension;
    node["tile_size"] = tileSize;
    node["apply_font"] = applyFont;
    node["keep_color_scheme"] = keepColorScheme;
    if (!colorSchemeScope.empty()) node["color_scheme_scope"] = colorSchemeScope;
    if (!selection.empty()) {
        YAML::Node list(YAML::NodeType::Sequence);
        for (const std::string& path : selection) list.push_back(path);
        node["selection"] = list;
    }
    return node;
}

DocumentView DocumentView::decode(const YAML::Node& node) {
    DocumentView v;
    if (!node || !node.IsMap()) return v;
    v.twirls = readFlags(node["twirls"]);
    const YAML::Node b = node["backdrop"];
    v.backdrop.kind = read<std::string>(b, "kind", "checker");
    v.backdrop.color = read<std::string>(b, "color", "#808080");
    v.backdrop.image = read<std::string>(b, "image", "");
    v.backdrop.scene = read<std::string>(b, "scene", "");
    const YAML::Node d = node["device"];
    v.device.profile = read<std::string>(d, "profile", "");
    v.device.orientation = read<std::string>(d, "orientation", "landscape_left");
    v.device.displayHeight = read(d, "display_height", 25.0);
    v.device.displayHeightPx = read(d, "display_height_px", false);
    v.device.anchor = std::clamp(read(d, "anchor", 4), 0, 8);
    const YAML::Node guides = node["guides"];
    if (guides && guides.IsSequence()) {
        for (const YAML::Node& entry : guides) {
            Guide g;
            g.vertical = read<std::string>(entry, "axis", "x") == "x";
            g.position = read(entry, "at", 0.0);
            v.guides.push_back(g);
        }
    }
    v.rulers = read(node, "rulers", true);
    v.showGuides = read(node, "show_guides", true);
    v.safeAreas = read(node, "safe_areas", true);
    v.pixelGrid = read(node, "pixel_grid", false);
    v.effectReach = read(node, "effect_reach", false);
    v.snap = read(node, "snap", true);
    v.fit = read(node, "fit", true);
    v.zoom = std::clamp(read(node, "zoom", 1.0f), 1.0f / 16.0f, 32.0f);
    v.panX = read(node, "pan_x", 0.0);
    v.panY = read(node, "pan_y", 0.0);
    v.previewAs = read<std::string>(node, "preview_as", "realtime");
    v.libraryTab = read<std::string>(node, "library_tab", "looks");
    v.libraryFamily = read<std::string>(node, "library_family", "");
    v.libraryQuery = read<std::string>(node, "library_query", "");
    v.libraryDimension = read<std::string>(node, "library_dimension", "all");
    v.tileSize = read<std::string>(node, "tile_size", "m");
    v.applyFont = read(node, "apply_font", false);
    v.keepColorScheme = read(node, "keep_color_scheme", true);
    v.colorSchemeScope = read<std::string>(node, "color_scheme_scope", "");
    const YAML::Node selection = node["selection"];
    if (selection && selection.IsSequence()) {
        for (const YAML::Node& entry : selection) {
            try {
                v.selection.push_back(entry.as<std::string>());
            } catch (const std::exception&) {
            }
        }
    }
    return v;
}

// ---- workspace ----

void TitleDesignerWorkspace::clear() {
    path.clear();
    recentDocs.clear();
    views.clear();
    unknown = YAML::Node();
}

void TitleDesignerWorkspace::load(const fs::path& projectPath) {
    clear();
    if (projectPath.empty()) return;
    path = Workspace::getUserDirectory(projectPath) / "title-designer.yaml";
    std::error_code ec;
    if (!fs::exists(path, ec)) return;
    YAML::Node root;
    try {
        root = YAML::LoadFile(path.string());
    } catch (const std::exception& e) {
        Out::warning("Title Designer: %s could not be read (%s); its view state starts empty", path.string().c_str(), e.what());
        return;
    }
    if (!root.IsMap()) return;
    unknown = YAML::Clone(root);
    const YAML::Node recent = root["recent"];
    if (recent && recent.IsSequence()) {
        for (const YAML::Node& entry : recent) {
            try {
                if (recentDocs.size() < MAX_RECENT) recentDocs.push_back(entry.as<std::string>());
            } catch (const std::exception&) {
            }
        }
    }
    const YAML::Node docs = root["documents"];
    if (docs && docs.IsMap()) {
        for (auto it = docs.begin(); it != docs.end(); ++it) {
            try {
                views[it->first.as<std::string>()] = DocumentView::decode(it->second);
            } catch (const std::exception&) {
            }
        }
    }
}

bool TitleDesignerWorkspace::save() const {
    if (path.empty()) return false;
    YAML::Node root = unknown && unknown.IsMap() ? YAML::Clone(unknown) : YAML::Node(YAML::NodeType::Map);
    YAML::Node recent(YAML::NodeType::Sequence);
    for (const std::string& doc : recentDocs) recent.push_back(doc);
    root["recent"] = recent;
    YAML::Node docs(YAML::NodeType::Map);
    for (const auto& [guid, view] : views) docs[guid] = view.encode();
    root["documents"] = docs;
    const std::string text = "# The Title Designer's view state for this checkout only (not committed).\n" + YAML::Dump(root) + "\n";
    std::error_code ec;
    safewrite::EditorInternal token(path.parent_path());
    FileUtils::createDirectories(path.parent_path(), ec);
    const io::WriteResult written = DocumentFiles::write(path, text);
    if (!written.ok()) {
        Out::error(DocumentFiles::saveFailureMessage(path, written));
        return false;
    }
    return true;
}

void TitleDesignerWorkspace::touch(const std::string& assetPath) {
    forget(assetPath);
    recentDocs.insert(recentDocs.begin(), assetPath);
    if (recentDocs.size() > MAX_RECENT) recentDocs.resize(MAX_RECENT);
}

void TitleDesignerWorkspace::forget(const std::string& assetPath) {
    recentDocs.erase(std::remove(recentDocs.begin(), recentDocs.end(), assetPath), recentDocs.end());
}

DocumentView& TitleDesignerWorkspace::view(const std::string& guid) {
    return views[guid];
}
