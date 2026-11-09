// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibraryProvider.h"

#include "AppSettings.h"
#include "Backend.h"
#include "TitleLibraryApply.h"
#include "TitleLibrarySearch.h"
#include "io/AtomicFile.h"
#include "texture/Texture.h"
#include "titleart/TitleArtOps.h"
#include "titleart/designer/TitleDesignerState.h"

#include "imgui.h"
#include "stb_image.h"
#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <list>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
using namespace jasperengine::editor::titledesigner;
namespace fs = std::filesystem;

namespace {

    LibraryKind kindOf(LibraryTab tab) {
        switch (tab) {
            case LibraryTab::Looks: return LibraryKind::Look;
            case LibraryTab::ColorSchemes: return LibraryKind::ColorScheme;
            case LibraryTab::Templates: return LibraryKind::Template;
        }
        return LibraryKind::Look;
    }

    std::vector<std::string> strings(const YAML::Node& node) {
        std::vector<std::string> out;
        if (node && node.IsSequence()) for (const YAML::Node& n : node) out.push_back(n.as<std::string>(""));
        return out;
    }

    std::string str(const titleart::Json& object, const char* key, const std::string& fallback = {}) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    std::string favoriteKey(LibraryKind kind, const std::string& ref) { return std::string(libraryKindName(kind)) + ":" + ref; }

    // the baked first-paint pictures (R32), uploaded on demand, a small LRU
    class BakedTiles {
    public:
        ImTextureID texture(const fs::path& file, float& width, float& height) {
            auto it = entries.find(file.string());
            if (it == entries.end()) {
                Entry e;
                int w = 0, h = 0, n = 0;
                unsigned char* pixels = stbi_load(file.string().c_str(), &w, &h, &n, 4);
                if (pixels) {
                    e.pixels.assign(pixels, pixels + (size_t)w * h * 4);
                    stbi_image_free(pixels);
                    e.texture.setData("editor:titlelibrary-baked:" + std::to_string(++uploads),
                                      TextureData(w, h, (unsigned int)e.pixels.size(), ColorFormat::RGBA, 4, e.pixels.data()));
                    e.texture.load();
                    e.width = w;
                    e.height = h;
                }
                it = entries.emplace(file.string(), std::move(e)).first;
                order.push_front(file.string());
                while (order.size() > MAX) {
                    entries[order.back()].texture.destroy();
                    entries.erase(order.back());
                    order.pop_back();
                }
            }
            width = (float)it->second.width;
            height = (float)it->second.height;
            return it->second.texture.getRender() ? Backend::getImGuiTexture(it->second.texture.getRender()) : 0;
        }
        void clear() {
            for (auto& [k, e] : entries) e.texture.destroy();
            entries.clear();
            order.clear();
        }

    private:
        struct Entry {
            Texture texture;
            std::vector<uint8_t> pixels;
            int width = 0, height = 0;
        };
        static constexpr size_t MAX = 240;
        std::map<std::string, Entry> entries;
        std::list<std::string> order;
        uint64_t uploads = 0;
    };

    class Provider : public TitleLibraryProvider {
    public:
        std::vector<LibraryItem> items(LibraryTab tab) override {
            const LibraryKind kind = kindOf(tab);
            Cached& c = cache[(int)kind];
            if (c.revision == TitleLibrary::get().revision()) return c.items;
            c.items.clear();
            const std::vector<std::string> featured = TitleLibrary::get().featuredTemplates();
            for (const LibraryItemInfo& info : TitleLibrary::get().items(kind)) {
                LibraryItem item;
                item.id = info.ref;
                item.name = info.name.any();
                item.family = info.family.empty() ? std::string("custom") : info.family;
                item.dimension = kind == LibraryKind::ColorScheme ? std::string() : info.dimension;
                item.user = info.scope != LibraryScope::BuiltIn;
                item.featured = std::find(featured.begin(), featured.end(), info.ref) != featured.end();
                if (kind == LibraryKind::Look) item.convertsTo = info.dimension;
                item.disabledReason = info.available ? std::string() : info.unavailableReason;
                item.terms = searchFields(info);
                c.items.push_back(std::move(item));
            }
            c.revision = TitleLibrary::get().revision();
            return c.items;
        }

        std::vector<std::string> families() override {
            std::vector<std::string> out;
            for (const LibraryFamily& f : TitleLibrary::get().families()) out.push_back(f.id);
            out.push_back("custom");
            return out;
        }

        titleart::Json applyOps(LibraryTab tab, const LibraryItem& item, const LibraryApplyOptions& options) override {
            const LibraryKind kind = kindOf(tab);
            if (kind == LibraryKind::Look && options.layers.empty()) {
                // nothing selected: a new text in that look (S82 R41)
                titleart::Json ops = titleart::Json::array();
                ops.push_back(titleart::Json{{"op", "add_layer"}, {"type", "text"}, {"name", item.name}, {"text", "TITLE"}});
                ops.push_back(titleart::Json{{"op", "apply_look"}, {"layer", item.name}, {"look", item.id}, {"apply_font", true},
                                   {"keep_color_scheme", options.keepColorScheme}});
                return ops;
            }
            const std::vector<std::string> layers = kind == LibraryKind::ColorScheme && options.wholeTitle ? std::vector<std::string>() : options.layers;
            std::vector<std::string> refs;
            for (const std::string& l : layers) refs.push_back("id:" + l);
            return applyOpsFor(kind, item.id, refs, options.applyFont, options.keepColorScheme);
        }

        std::string stepName(LibraryTab tab, const LibraryItem& item, const LibraryApplyOptions& options) override {
            const LibraryKind kind = kindOf(tab);
            if (kind == LibraryKind::Template) return "Apply Template " + item.name;
            if (kind == LibraryKind::ColorScheme) {
                if (options.wholeTitle || options.layers.empty()) return "Apply Color Scheme " + item.name + " to Whole Title";
                return "Apply Color Scheme " + item.name + (options.layers.size() > 1 ? " (" + std::to_string(options.layers.size()) + " layers)" : "");
            }
            return "Apply Look " + item.name + (options.layers.size() > 1 ? " (" + std::to_string(options.layers.size()) + " layers)" : "");
        }

        LibraryTile tile(LibraryTab tab, const LibraryItem& item, const std::string& targetKey, float width, float height, bool visible) override {
            LibraryTile out;
            if (source) out = source(tab, item, width, height, visible);
            if (out.texture || !out.failed.empty()) return out;
            // the baked first paint while the live tile renders (R33)
            // S82's target key carries the layer's text after its last '|' (the window's tile key)
            const size_t bar = targetKey.rfind('|');
            const std::string text = kindOf(tab) == LibraryKind::Template ? std::string("en")
                                                                          : sampleKindOf(bar == std::string::npos ? targetKey : targetKey.substr(bar + 1));
            float w = 0, h = 0;
            const ImTextureID baked = bakedTiles.texture(bakedThumbnailPath(kindOf(tab), item.id, text), w, h);
            if (baked) {
                out.texture = baked;
                out.width = w;
                out.height = h;
                out.live = false;
            }
            return out;
        }

        std::string titleLayer(const titleart::TitleArtDoc& doc) override {
            const titleart::Json& exposed = doc.root.contains("exposed") ? doc.root["exposed"] : titleart::Json::array();
            for (const titleart::Json& e : exposed) {
                if (str(e, "type") == "text" && str(e, "name") == "text" && e.contains("targets") && !e["targets"].empty()) return str(e["targets"][0], "layer");
            }
            return {};
        }

        bool isFavorite(LibraryTab tab, const std::string& id) override { return prefs().isFavorite(kindOf(tab), id); }
        void setFavorite(LibraryTab tab, const std::string& id, bool favorite) override {
            LibraryPrefs p = LibraryPrefs::load();
            p.setFavorite(kindOf(tab), id, favorite);
            p.dropMissing();
            p.save();
            loaded = false;
        }
        std::vector<std::string> recents(LibraryTab tab) override {
            const auto& r = prefs().recents;
            auto it = r.find(libraryKindName(kindOf(tab)));
            return it == r.end() ? std::vector<std::string>() : it->second;
        }
        void recordApply(LibraryTab tab, const std::string& id) override {
            LibraryPrefs p = LibraryPrefs::load();
            p.recordApply(kindOf(tab), id);
            p.save();
            loaded = false;
        }

        std::string structureNote(const titleart::TitleArtDoc& before, const titleart::TitleArtDoc& after) override {
            auto count = [](const titleart::TitleArtDoc& d, titleart::LayerType type) {
                int n = 0;
                titleart::forEachLayer(d.root, [&](const titleart::Json& l, const titleart::Json*, int) {
                    if (titleart::layerTypeOf(l) == type) n++;
                    return true;
                });
                return n;
            };
            const int groups = count(after, titleart::LayerType::Group) - count(before, titleart::LayerType::Group);
            const int generators = count(after, titleart::LayerType::Generator) - count(before, titleart::LayerType::Generator);
            std::string note;
            if (groups > 0) note += groups == 1 ? "a Group" : std::to_string(groups) + " Groups";
            if (generators > 0) note += std::string(note.empty() ? "" : " and ") + (generators == 1 ? "a Generator layer" : std::to_string(generators) + " Generator layers");
            return note.empty() ? std::string() : "added " + note;
        }

        bool drawSaveAs(LibraryTab tab, TitleDesignerState& state) override {
            const LibraryKind kind = kindOf(tab);
            if (dialogKind != (int)kind) {
                dialogKind = (int)kind;
                nameBuffer[0] = idBuffer[0] = tagsBuffer[0] = 0;
                std::snprintf(familyBuffer, sizeof(familyBuffer), "custom");
                scopeIndex = 0;
                dialogError.clear();
            }
            const std::vector<std::string> layers = state.selectedLayers();
            ImGui::TextUnformatted((std::string("Save As ") + libraryKindLabel(kind)).c_str());
            ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer));
            ImGui::InputText("ID", idBuffer, sizeof(idBuffer));
            const char* scopes[] = {"Project", "My Library"};
            ImGui::Combo("Scope", &scopeIndex, scopes, 2);
            if (kind != LibraryKind::ColorScheme) ImGui::InputText("Family", familyBuffer, sizeof(familyBuffer));
            ImGui::InputText("Tags", tagsBuffer, sizeof(tagsBuffer));
            titleart::Json op = titleart::Json{{"name", std::string(nameBuffer)}, {"scope", scopeIndex == 0 ? "project" : "user"}};
            if (idBuffer[0]) op["id"] = std::string(idBuffer);
            if (kind != LibraryKind::ColorScheme) op["family"] = std::string(familyBuffer);
            titleart::Json tags = titleart::Json::array();
            std::string tag;
            for (const char* p = tagsBuffer;; p++) {
                if (*p == ',' || *p == ' ' || *p == 0) {
                    if (!tag.empty()) tags.push_back(tag);
                    tag.clear();
                    if (*p == 0) break;
                } else {
                    tag += *p;
                }
            }
            op["tags"] = tags;
            if (kind == LibraryKind::Look) {
                op["op"] = "save_look";
                if (!layers.empty()) op["layer"] = "id:" + layers.front();
            } else if (kind == LibraryKind::ColorScheme) {
                op["op"] = "save_color_scheme";
                op.erase("tags");
                if (!layers.empty()) op["layer"] = "id:" + layers.front();
            } else {
                op["op"] = "save_template";
                op.erase("tags");
            }
            // the dialog validates with the op itself (a dry run of the same function)
            const TitleOpsResult check = TitleArtOps::apply(state.committed(), titleart::Json::array({op}));
            if (!check.ok) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", check.error().c_str());
            else if (!dialogError.empty()) ImGui::TextColored(ImVec4(1, 0.45f, 0.35f, 1), "%s", dialogError.c_str());
            bool closed = false;
            ImGui::BeginDisabled(!check.ok);
            if (ImGui::Button("Save")) {
                std::string error;
                if (state.commit(titleart::Json::array({op}), check.undoName, &error)) {
                    closed = true;
                    TitleLibrary::get().invalidate();
                } else {
                    dialogError = error;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) closed = true;
            if (closed) dialogKind = -1;
            return closed;
        }

        bool renameItem(LibraryTab tab, const std::string& id, const std::string& name, std::string* error) override {
            LibraryError e;
            const LibraryItemInfo* info = TitleLibrary::get().info(kindOf(tab), id, &e);
            if (!info || info->scope == LibraryScope::BuiltIn) {
                if (error) *error = info ? "built-in items cannot be renamed" : e.message;
                return false;
            }
            titleart::Json root = TitleLibrary::get().itemJson(*info);
            titleart::Json& identity = kindOf(tab) == LibraryKind::Template ? root["template"] : root;
            identity["name"] = decodeLocalized(titleart::Json(name)).toJson();
            std::string text;
            if (kindOf(tab) == LibraryKind::Look) {
                LookParseResult parsed = parseLookJson(root);
                if (!parsed.ok) return false;
                text = writeLook(parsed.look);
            } else if (kindOf(tab) == LibraryKind::ColorScheme) {
                titleart::ColorScheme s;
                if (!titleart::parseColorScheme(root, s, nullptr)) return false;
                text = titleart::writeColorSchemeText(s);
            } else {
                TemplateParseResult parsed = parseTemplate(root.dump());
                if (!parsed.ok) return false;
                text = writeTemplate(parsed.tmpl);
            }
            const bool written = io::writeFileAtomic(info->file, text).ok();
            TitleLibrary::get().invalidate();
            return written;
        }

        bool deleteItem(LibraryTab tab, const std::string& id, std::string* error) override {
            TitleOpsResult r = runLibraryBatch(project(), titleart::Json::array({titleart::Json{{"op", "delete_library_item"}, {"ref", id}, {"kind", libraryKindName(kindOf(tab))}}}));
            if (!r.ok && error) *error = r.error();
            return r.ok;
        }

        bool importFiles(const std::vector<std::string>& files, std::string* error) override {
            // dropped .titlelook / .titlecolors / .titletemplate files join the project scope (R17)
            titleart::Json ops = titleart::Json::array();
            std::vector<std::pair<fs::path, LibraryKind>> plan;
            for (const std::string& f : files) {
                const std::string ext = fs::path(f).extension().string();
                LibraryKind kind;
                if (ext == TITLE_LOOK_EXTENSION) kind = LibraryKind::Look;
                else if (ext == TITLE_COLORS_EXTENSION) kind = LibraryKind::ColorScheme;
                else if (ext == TITLE_TEMPLATE_EXTENSION) kind = LibraryKind::Template;
                else {
                    if (error) *error = "'" + f + "' is not a library file (.titlelook, .titlecolors, .titletemplate)";
                    return false;
                }
                plan.push_back({f, kind});
            }
            if (TitleLibrary::get().projectDir().empty()) {
                if (error) *error = "no project is open";
                return false;
            }
            for (const auto& [file, kind] : plan) {
                std::ifstream in(file, std::ios::binary);
                std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const std::string slug = slugOf(file.stem().string());
                const fs::path target = TitleLibrary::get().scopeDir(LibraryScope::Project, kind) / (slug + libraryExtension(kind));
                std::error_code ec;
                if (fs::exists(target, ec)) {
                    if (error) *error = "'" + std::string(libraryScopeName(LibraryScope::Project)) + "/" + slug + "' exists";
                    return false;
                }
                // validated by its own reader before it joins
                bool ok = false;
                if (kind == LibraryKind::Look) ok = parseLook(text).ok;
                else if (kind == LibraryKind::ColorScheme) ok = titleart::parseColorScheme(titleart::Json::parse(text, nullptr, false), *std::make_unique<titleart::ColorScheme>(), nullptr);
                else ok = parseTemplate(text).ok;
                if (!ok) {
                    if (error) *error = "'" + file.string() + "' does not load as a " + libraryKindLabel(kind);
                    return false;
                }
                const io::WriteResult written = io::writeFileAtomic(target, text);
                if (!written.ok()) {
                    if (error) *error = "'" + target.string() + "': " + io::describeWriteError(written);
                    return false;
                }
            }
            TitleLibrary::get().invalidate();
            return true;
        }

        void attachTileSource(TileSource s) override { source = std::move(s); }
        void cancelAll() override {}
        void deviceLost() override { bakedTiles.clear(); }

    private:
        struct Cached {
            uint64_t revision = 0;
            std::vector<LibraryItem> items;
        };
        Cached cache[3];
        TileSource source;
        BakedTiles bakedTiles;
        LibraryPrefs cachedPrefs;
        bool loaded = false;
        int dialogKind = -1;
        char nameBuffer[128] = {};
        char idBuffer[64] = {};
        char familyBuffer[32] = {};
        char tagsBuffer[256] = {};
        int scopeIndex = 0;
        std::string dialogError;

        const LibraryPrefs& prefs() {
            if (!loaded) {
                cachedPrefs = LibraryPrefs::load();
                loaded = true;
            }
            return cachedPrefs;
        }
        Project* project() const { return currentProject(); }
    };

}

// ---- LibraryPrefs ----

LibraryPrefs LibraryPrefs::load() {
    LibraryPrefs p;
    const YAML::Node node = AppSettings::getTitleLibrarySettings();
    p.favorites = strings(node["favorites"]);
    if (node["recents"] && node["recents"].IsMap()) {
        for (const auto& kv : node["recents"]) p.recents[kv.first.as<std::string>("")] = strings(kv.second);
    }
    p.applyFont = node["apply_font"] ? node["apply_font"].as<bool>(false) : false;
    p.keepColorScheme = node["keep_color_scheme"] ? node["keep_color_scheme"].as<bool>(true) : true;
    p.foldedSections = strings(node["folded_sections"]);
    return p;
}

void LibraryPrefs::save() const {
    YAML::Node node(YAML::NodeType::Map);
    for (const std::string& f : favorites) node["favorites"].push_back(f);
    if (favorites.empty()) node["favorites"] = YAML::Node(YAML::NodeType::Sequence);
    YAML::Node r(YAML::NodeType::Map);
    for (const auto& [kind, refs] : recents) {
        YAML::Node list(YAML::NodeType::Sequence);
        for (const std::string& ref : refs) list.push_back(ref);
        r[kind] = list;
    }
    node["recents"] = r;
    node["apply_font"] = applyFont;
    node["keep_color_scheme"] = keepColorScheme;
    YAML::Node folded(YAML::NodeType::Sequence);
    for (const std::string& f : foldedSections) folded.push_back(f);
    node["folded_sections"] = folded;
    AppSettings::setTitleLibrarySettings(node);
    AppSettings::saveSettings();
}

bool LibraryPrefs::isFavorite(LibraryKind kind, const std::string& ref) const {
    return std::find(favorites.begin(), favorites.end(), favoriteKey(kind, ref)) != favorites.end();
}

void LibraryPrefs::setFavorite(LibraryKind kind, const std::string& ref, bool favorite) {
    const std::string key = favoriteKey(kind, ref);
    favorites.erase(std::remove(favorites.begin(), favorites.end(), key), favorites.end());
    if (favorite) favorites.push_back(key);
}

void LibraryPrefs::recordApply(LibraryKind kind, const std::string& ref) {
    std::vector<std::string>& list = recents[libraryKindName(kind)];
    list.erase(std::remove(list.begin(), list.end(), ref), list.end());
    list.insert(list.begin(), ref);
    if (list.size() > TITLE_LIBRARY_RECENTS) list.resize(TITLE_LIBRARY_RECENTS);
}

void LibraryPrefs::dropMissing() {
    favorites.erase(std::remove_if(favorites.begin(), favorites.end(), [](const std::string& key) {
        const size_t colon = key.find(':');
        LibraryKind kind;
        if (colon == std::string::npos || !libraryKindFromName(key.substr(0, colon), kind)) return true;
        return TitleLibrary::get().info(kind, key.substr(colon + 1), nullptr) == nullptr;
    }), favorites.end());
}

fs::path titlelibrary::bakedThumbnailPath(LibraryKind kind, const std::string& ref, const std::string& text) {
    std::string name = ref;
    std::replace(name.begin(), name.end(), '/', '.');
    return TitleLibrary::packDir() / "thumbnails" / libraryKindName(kind) / (name + "." + text + ".png");
}

std::string titlelibrary::sampleKindOf(const std::string& text) {
    if (hasJapaneseScript(text)) return "kanji";
    bool digits = !text.empty();
    for (unsigned char c : text) digits = digits && ((c >= '0' && c <= '9') || c == ' ' || c == ',' || c == '.');
    return digits ? "digits" : "latin";
}

std::shared_ptr<TitleLibraryProvider> titlelibrary::makeTitleLibraryProvider() {
    return std::make_shared<Provider>();
}
