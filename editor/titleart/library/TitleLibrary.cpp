// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibrary.h"

#include "AppSettings.h"
#include "search/SearchScorer.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"
#include "util/FileUtils.h"

#include "titleart_library.h"

#include "zstd.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
namespace fs = std::filesystem;

namespace {

    const char* const KIND_DIRS_BUILTIN[] = {"looks", "color-schemes", "templates"};
    const char* const KIND_DIRS_PROJECT[] = {"Looks", "ColorSchemes", "Templates"};

    size_t kindIndex(LibraryKind kind) { return static_cast<size_t>(kind); }

    bool readText(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    // ---- the embedded archive: a zstd-compressed tar (cmake -E tar --zstd --format=gnutar) ----

    std::string decompress(const unsigned char* data, size_t size) {
        std::string out;
        ZSTD_DStream* stream = ZSTD_createDStream();
        if (!stream) return out;
        ZSTD_initDStream(stream);
        ZSTD_inBuffer in = {data, size, 0};
        std::vector<char> buffer(ZSTD_DStreamOutSize());
        while (in.pos < in.size) {
            ZSTD_outBuffer o = {buffer.data(), buffer.size(), 0};
            const size_t r = ZSTD_decompressStream(stream, &o, &in);
            if (ZSTD_isError(r)) {
                out.clear();
                break;
            }
            out.append(buffer.data(), o.pos);
            if (r == 0 && in.pos >= in.size) break;
        }
        ZSTD_freeDStream(stream);
        return out;
    }

    uint64_t octal(const char* p, size_t n) {
        uint64_t v = 0;
        for (size_t i = 0; i < n && p[i]; i++) {
            if (p[i] < '0' || p[i] > '7') continue;
            v = v * 8 + static_cast<uint64_t>(p[i] - '0');
        }
        return v;
    }

    std::map<std::string, std::string> untar(const std::string& tar) {
        std::map<std::string, std::string> files;
        size_t at = 0;
        std::string longName;
        while (at + 512 <= tar.size()) {
            const char* h = tar.data() + at;
            if (h[0] == 0) break;
            std::string name(h, strnlen(h, 100));
            const char type = h[156];
            if (std::memcmp(h + 257, "ustar", 5) == 0 && h[345]) name = std::string(h + 345, strnlen(h + 345, 155)) + "/" + name;
            const uint64_t size = octal(h + 124, 12);
            at += 512;
            if (at + size > tar.size()) break;
            if (type == 'L') {
                longName.assign(tar.data() + at, strnlen(tar.data() + at, size));
            } else if (type == '0' || type == 0) {
                if (!longName.empty()) name = longName;
                longName.clear();
                if (name.rfind("./", 0) == 0) name = name.substr(2);
                files[name] = tar.substr(at, size);
            } else {
                longName.clear();
            }
            at += (size + 511) / 512 * 512;
        }
        return files;
    }

    std::string folderSignature(const fs::path& dir, const char* extension) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return {};
        std::vector<std::string> entries;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != extension) continue;
            const auto size = e.file_size(ec);
            const auto time = fs::last_write_time(e.path(), ec).time_since_epoch().count();
            entries.push_back(e.path().filename().string() + ":" + std::to_string(size) + ":" + std::to_string(time));
        }
        std::sort(entries.begin(), entries.end());
        std::string out;
        for (const std::string& e : entries) out += e + "\n";
        return out;
    }

    std::vector<std::string> strings(const Json& list) {
        std::vector<std::string> out;
        if (list.is_array()) for (const Json& v : list) if (v.is_string()) out.push_back(v.get<std::string>());
        return out;
    }

    std::string joined(const Json& list) {
        std::string out;
        for (const std::string& s : strings(list)) out += (out.empty() ? "" : " ") + s;
        return out;
    }

    // ---- what an item uses (§6.13) ----

    struct Terms {
        std::set<std::string> uses, features;
        int cost = 0;
        bool generators = false;
    };

    void addEffect(Terms& t, const std::string& type) {
        if (type.empty()) return;
        t.uses.insert("effect:" + type);
        if (const titleart::EffectDef* def = titleart::findEffect(type)) t.cost = std::max(t.cost, (int)def->costClass);
    }
    void addGenerator(Terms& t, const std::string& type) {
        if (type.empty()) return;
        t.uses.insert("generator:" + type);
        t.generators = true;
        if (const titleart::GeneratorDef* def = titleart::findGenerator(type)) t.cost = std::max(t.cost, (int)def->costClass);
    }
    void effectsOf(Terms& t, const Json& list) {
        if (!list.is_array()) return;
        for (const Json& e : list) if (e.is_object()) addEffect(t, e.value("type", std::string()));
    }

    // materials (2D) and their parts: patterns, textures, bevels...
    void walkMaterial(Terms& t, const Json& m) {
        if (!m.is_object()) return;
        auto enabled = [&](const char* key) {
            return m.contains(key) && m[key].is_object() && m[key].value("enabled", false);
        };
        if (enabled("bevel")) t.features.insert("bevel");
        if (enabled("finish")) t.features.insert("finish");
        if (enabled("glass")) t.features.insert("glass");
        if (enabled("reflection")) t.features.insert("reflection");
        for (const char* key : {"pattern", "texture"}) {
            if (!m.contains(key) || !m[key].is_object()) continue;
            const std::string kind = m[key].value("kind", std::string("none"));
            if (kind.empty() || kind == "none") continue;
            t.uses.insert(std::string(key) + ":" + kind);
            t.features.insert(std::string(key) + ":" + kind);
        }
    }

    void walkStyle2D(Terms& t, const Json& style) {
        if (!style.is_object()) return;
        if (style.contains("face")) walkMaterial(t, style["face"]);
        if (style.contains("rims") && style["rims"].is_array()) {
            for (const Json& r : style["rims"]) if (r.is_object() && r.contains("material")) walkMaterial(t, r["material"]);
        }
        if (style.value("knockout", false)) t.features.insert("knockout");
        if (style.contains("erode") && style["erode"].is_object() && style["erode"].value("enabled", false)) t.features.insert("erode");
    }

    void walkMaterial3D(Terms& t, const Json& m) {
        if (!m.is_object()) return;
        const std::string preset = m.value("preset", std::string());
        if (!preset.empty() && preset != "custom") t.uses.insert("material:" + preset);
        if (m.value("transmission", 0.0) > 0) t.features.insert("glass");
        if (m.contains("pattern") && m["pattern"].is_object()) {
            const std::string kind = m["pattern"].value("kind", std::string("none"));
            if (!kind.empty() && kind != "none") {
                t.uses.insert("pattern:" + kind);
                t.features.insert("pattern:" + kind);
            }
        }
    }

    void walkStyle3D(Terms& t, const Json& style) {
        t.uses.insert("3d");
        t.features.insert("3d");
        t.cost = std::max(t.cost, 3);
        if (!style.is_object()) return;
        const std::string mode = style.value("mode", std::string("extrude"));
        t.features.insert(mode == "inflate" ? "inflate" : "extrude");
        if (style.contains("bevel") && style["bevel"].is_object()) t.features.insert("bevel");
        for (const char* key : {"face", "side", "bevel_material"}) {
            if (style.contains(key)) walkMaterial3D(t, style[key]);
        }
        if (style.contains("bands") && style["bands"].is_array()) {
            for (const Json& b : style["bands"]) if (b.is_object() && b.contains("material")) walkMaterial3D(t, b["material"]);
        }
    }

    void walkPartEffects(Terms& t, const Json& parts) {
        if (!parts.is_object()) return;
        for (auto it = parts.begin(); it != parts.end(); ++it) effectsOf(t, it.value());
    }

    void walkLayer(Terms& t, const Json& layer) {
        if (!layer.is_object()) return;
        effectsOf(t, layer.contains("effects") ? layer["effects"] : Json());
        if (layer.contains("part_effects")) walkPartEffects(t, layer["part_effects"]);
        const std::string type = layer.value("type", std::string());
        if (type == "generator" && layer.contains("generator") && layer["generator"].is_object()) {
            addGenerator(t, layer["generator"].value("type", std::string()));
        }
        if (layer.value("dimension", std::string("2d")) == "3d") walkStyle3D(t, layer.contains("style_3d") ? layer["style_3d"] : Json());
        else if (layer.contains("style")) walkStyle2D(t, layer["style"]);
        if (layer.contains("children") && layer["children"].is_array()) for (const Json& c : layer["children"]) walkLayer(t, c);
    }

    const std::map<std::string, std::string>& requirementOwners() {
        static const std::map<std::string, std::string> owners = {
            {"effect", "S83"}, {"generator", "S83"}, {"3d", "S86"}, {"material", "S86"}, {"pattern", "S80"}, {"texture", "S80"}};
        return owners;
    }

    bool hasRequirement(const std::string& r) {
        const size_t colon = r.find(':');
        const std::string what = colon == std::string::npos ? r : r.substr(0, colon);
        const std::string name = colon == std::string::npos ? std::string() : r.substr(colon + 1);
        const titleart::TitleArtSchema& schema = titleart::TitleArtSchema::get();
        if (what == "effect") return titleart::findEffect(name) != nullptr;
        if (what == "generator") return titleart::findGenerator(name) != nullptr;
        if (what == "3d") return schema.kind("style_3d") != nullptr;
        if (what == "material") {
            const titleart::SchemaKind* k = schema.kind("material_3d");
            const titleart::SchemaMember* m = k ? schema.member(*k, "preset") : nullptr;
            return m && std::find(m->options.begin(), m->options.end(), name) != m->options.end();
        }
        if (what == "pattern" || what == "texture") {
            const titleart::SchemaKind* k = schema.kind(what);
            const titleart::SchemaMember* m = k ? schema.member(*k, "kind") : nullptr;
            return m && std::find(m->options.begin(), m->options.end(), name) != m->options.end();
        }
        return true;
    }

}

const char* titlelibrary::libraryKindName(LibraryKind kind) {
    switch (kind) {
        case LibraryKind::Look: return "looks";
        case LibraryKind::ColorScheme: return "color_schemes";
        case LibraryKind::Template: return "templates";
    }
    return "looks";
}

const char* titlelibrary::libraryKindLabel(LibraryKind kind) {
    switch (kind) {
        case LibraryKind::Look: return "Look";
        case LibraryKind::ColorScheme: return "Color Scheme";
        case LibraryKind::Template: return "Template";
    }
    return "Look";
}

bool titlelibrary::libraryKindFromName(std::string_view name, LibraryKind& out) {
    for (LibraryKind k : LIBRARY_KINDS) {
        if (name == libraryKindName(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

const char* titlelibrary::libraryScopeName(LibraryScope scope) {
    switch (scope) {
        case LibraryScope::BuiltIn: return "builtin";
        case LibraryScope::Project: return "project";
        case LibraryScope::User: return "user";
    }
    return "builtin";
}

const char* titlelibrary::libraryExtension(LibraryKind kind) {
    switch (kind) {
        case LibraryKind::Look: return TITLE_LOOK_EXTENSION;
        case LibraryKind::ColorScheme: return TITLE_COLORS_EXTENSION;
        case LibraryKind::Template: return TITLE_TEMPLATE_EXTENSION;
    }
    return TITLE_LOOK_EXTENSION;
}

std::string titlelibrary::requirementReason(const std::string& r) {
    const size_t colon = r.find(':');
    const std::string what = colon == std::string::npos ? r : r.substr(0, colon);
    const std::string name = colon == std::string::npos ? std::string() : r.substr(colon + 1);
    auto owner = requirementOwners().find(what);
    const std::string by = owner == requirementOwners().end() ? std::string() : " (" + owner->second + ")";
    if (what == "3d") return "needs 3D titles" + by;
    if (what == "material") return "needs 3D material " + name + by;
    return "needs " + what + " " + name + by;
}

void titlelibrary::deriveItemTerms(LibraryKind kind, const Json& item, LibraryItemInfo& out) {
    Terms t;
    if (kind == LibraryKind::Look) {
        if (item.value("dimension", std::string("2d")) == "3d") walkStyle3D(t, item.contains("style_3d") ? item["style_3d"] : Json());
        else if (item.contains("style")) walkStyle2D(t, item["style"]);
        effectsOf(t, item.contains("effects") ? item["effects"] : Json());
        effectsOf(t, item.contains("group_effects") ? item["group_effects"] : Json());
        if (item.contains("part_effects")) walkPartEffects(t, item["part_effects"]);
        if (item.contains("generators") && item["generators"].is_array()) {
            for (const Json& g : item["generators"]) {
                if (!g.is_object()) continue;
                addGenerator(t, g.value("type", std::string()));
                effectsOf(t, g.contains("effects") ? g["effects"] : Json());
            }
        }
    } else if (kind == LibraryKind::Template) {
        if (item.contains("layers") && item["layers"].is_array()) for (const Json& l : item["layers"]) walkLayer(t, l);
    } else {
        // a scheme uses nothing; its pattern marks are a feature
        if (item.contains("pattern") && item["pattern"].is_object()) {
            const std::string kindName = item["pattern"].value("kind", std::string("none"));
            if (kindName != "none") t.features.insert("pattern:" + kindName);
        }
        if (item.value("spectrum", false)) t.features.insert("spectrum");
    }
    if (t.generators) t.features.insert("generators");
    for (const std::string& u : t.uses) {
        if (u.rfind("effect:", 0) == 0) t.features.insert(u.substr(7));
    }
    out.uses.assign(t.uses.begin(), t.uses.end());
    out.features.assign(t.features.begin(), t.features.end());
    out.cost = std::string("C") + std::to_string(std::clamp(t.cost, 0, 4));
}

// ---- TitleLibrary ----

TitleLibrary& TitleLibrary::get() {
    static TitleLibrary library;
    return library;
}

fs::path TitleLibrary::packDir() {
    return FileUtils::getTitleArtLibraryDir();
}

void TitleLibrary::setProject(const fs::path& projectDir, const fs::path& assetsDir) {
    assets = assetsDir.empty() && !projectDir.empty() ? projectDir / "assets" : assetsDir;
    if (projectDir == project) return;
    project = projectDir;
    for (auto& [kind, s] : state) {
        s.project = ScopeState();
        s.dirty = true;
    }
}

fs::path TitleLibrary::userDir() const {
    if (!userOverride.empty()) return userOverride;
    return AppSettings::getConfigDirectory() / "title-library";
}

void TitleLibrary::setUserDirForTesting(const fs::path& dir) {
    userOverride = dir;
    for (auto& [kind, s] : state) {
        s.user = ScopeState();
        s.dirty = true;
    }
}

fs::path TitleLibrary::scopeDir(LibraryScope scope, LibraryKind kind) const {
    switch (scope) {
        case LibraryScope::BuiltIn: return {};
        case LibraryScope::Project:
            if (project.empty()) return {};
            return project / "EditorSettings" / "TitleLibrary" / KIND_DIRS_PROJECT[kindIndex(kind)];
        case LibraryScope::User: return userDir() / KIND_DIRS_BUILTIN[kindIndex(kind)];
    }
    return {};
}

const std::map<std::string, std::string>& TitleLibrary::builtinFiles() {
    if (!embeddedLoaded) {
        embedded = untar(decompress(titleart_library_archive, titleart_library_archive_size));
        embeddedLoaded = true;
    }
    return embedded;
}

const std::map<std::string, std::string>& TitleLibrary::builtinSource() {
    return overrideBuiltins ? builtinOverride : builtinFiles();
}

void TitleLibrary::setBuiltinsForTesting(std::map<std::string, std::string> files) {
    overrideBuiltins = !files.empty();
    builtinOverride = std::move(files);
    manifestLoaded = false;
    invalidate();
}

void TitleLibrary::setRequirementCheckForTesting(std::function<bool(const std::string&)> check) {
    requirementCheck = std::move(check);
    invalidate();
}

void TitleLibrary::invalidate() {
    changes++;
    state.clear();
    lookCache.clear();
    schemeCache.clear();
    templateCache.clear();
}

const Json& TitleLibrary::manifest() {
    if (!manifestLoaded) {
        manifestLoaded = true;
        manifestJson = Json::object();
        auto it = builtinSource().find("manifest.json");
        if (it != builtinSource().end()) {
            Json parsed = Json::parse(it->second, nullptr, false);
            if (parsed.is_object()) manifestJson = std::move(parsed);
        }
        familyList.clear();
        if (manifestJson.contains("families") && manifestJson["families"].is_array()) {
            for (const Json& f : manifestJson["families"]) {
                if (!f.is_object()) continue;
                familyList.push_back({f.value("id", std::string()), decodeLocalized(f.contains("name") ? f["name"] : Json())});
            }
        }
    }
    return manifestJson;
}

const std::vector<LibraryFamily>& TitleLibrary::families() {
    manifest();
    return familyList;
}

const LibraryFamily* TitleLibrary::family(std::string_view id) {
    for (const LibraryFamily& f : families()) if (f.id == id) return &f;
    return nullptr;
}

std::vector<std::string> TitleLibrary::featuredTemplates() {
    return strings(manifest().contains("featured") ? manifest()["featured"] : Json());
}

void TitleLibrary::computeAvailability(LibraryItemInfo& item) {
    item.requirements.clear();
    for (const std::string& u : item.uses) {
        const bool has = requirementCheck ? requirementCheck(u) : hasRequirement(u);
        if (!has) item.requirements.push_back(u);
    }
    item.available = item.requirements.empty();
    item.unavailableReason.clear();
    for (const std::string& r : item.requirements) {
        item.unavailableReason += (item.unavailableReason.empty() ? "" : "; ") + requirementReason(r);
    }
}

bool TitleLibrary::indexItem(LibraryKind kind, LibraryScope scope, const std::string& text, LibraryItemInfo& out) {
    Json root = Json::parse(text, nullptr, false);
    if (!root.is_object()) {
        out.loadWarnings.push_back("not a JSON object");
        return false;
    }
    out.kind = kind;
    out.scope = scope;
    const Json* identity = &root;
    if (kind == LibraryKind::Template) {
        if (!root.contains("template") || !root["template"].is_object()) {
            out.loadWarnings.push_back("no `template` member");
            return false;
        }
        identity = &root["template"];
    } else {
        const char* format = kind == LibraryKind::Look ? TITLE_LOOK_FORMAT : titleart::TITLE_COLORS_FORMAT;
        if (root.value("format", std::string()) != format) {
            out.loadWarnings.push_back(std::string("`format` is not \"") + format + "\"");
            return false;
        }
        const int newest = kind == LibraryKind::Look ? TITLE_LOOK_VERSION : titleart::TITLE_COLORS_VERSION;
        if (root.value("version", 1) > newest) out.readOnly = true;
    }
    const Json& id = *identity;
    out.revision = id.value("revision", 1);
    out.name = decodeLocalized(id.contains("name") ? id["name"] : Json());
    out.description = decodeLocalized(id.contains("description") ? id["description"] : Json());
    out.tags = strings(id.contains("tags") ? id["tags"] : Json());
    if (id.contains("synonyms") && id["synonyms"].is_object()) {
        out.synonymsText.en = joined(id["synonyms"].contains("en") ? id["synonyms"]["en"] : Json());
        out.synonymsText.ja = joined(id["synonyms"].contains("ja") ? id["synonyms"]["ja"] : Json());
    }
    if (kind == LibraryKind::ColorScheme) {
        out.familyHint = strings(id.contains("family_hint") ? id["family_hint"] : Json());
        out.family = out.familyHint.empty() ? std::string() : out.familyHint.front();
    } else {
        out.family = id.value("family", std::string("custom"));
    }
    if (kind == LibraryKind::Look) {
        out.dimension = root.value("dimension", std::string("2d"));
        if (root.contains("font") && root["font"].is_object()) {
            out.fontPair = {root["font"].value("latin", std::string()), root["font"].value("ja", std::string())};
        }
    }
    deriveItemTerms(kind, root, out);
    if (kind == LibraryKind::Template) out.dimension = std::find(out.uses.begin(), out.uses.end(), "3d") != out.uses.end() ? "3d" : "2d";
    computeAvailability(out);
    return true;
}

void TitleLibrary::indexBuiltins(LibraryKind kind, KindState& s) {
    s.builtins.clear();
    const std::string prefix = std::string(KIND_DIRS_BUILTIN[kindIndex(kind)]) + "/";
    const std::string extension = libraryExtension(kind);
    for (const auto& [path, text] : builtinSource()) {
        if (path.rfind(prefix, 0) != 0 || path.size() <= extension.size() || path.compare(path.size() - extension.size(), extension.size(), extension) != 0) {
            continue;
        }
        LibraryItemInfo info;
        info.ref = path.substr(prefix.size(), path.size() - prefix.size() - extension.size());
        info.builtinPath = path;
        if (!indexItem(kind, LibraryScope::BuiltIn, text, info)) continue;
        info.readOnly = true;
        s.builtins.push_back(std::move(info));
    }
    s.builtinsIndexed = true;
}

void TitleLibrary::scanScope(LibraryKind kind, LibraryScope scope, ScopeState& s) {
    const fs::path dir = scopeDir(scope, kind);
    const std::string extension = libraryExtension(kind);
    const std::string signature = dir.empty() ? std::string() : folderSignature(dir, extension.c_str());
    if (s.scanned && signature == s.signature) return;
    s.items.clear();
    s.signature = signature;
    s.scanned = true;
    if (dir.empty() || signature.empty()) return;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == extension) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        std::string text;
        if (!readText(file, text)) continue;
        LibraryItemInfo info;
        const std::string slug = file.stem().string();
        info.ref = std::string(libraryScopeName(scope)) + "/" + slug;
        info.file = file;
        info.stamp = std::to_string(fs::file_size(file, ec)) + ":" + std::to_string(fs::last_write_time(file, ec).time_since_epoch().count());
        if (!isLibrarySlug(slug)) info.loadWarnings.push_back("the file name is not a slug ([a-z0-9-]{1,48})");
        if (!indexItem(kind, scope, text, info)) {
            info.available = false;
            info.unavailableReason = "does not load: " + (info.loadWarnings.empty() ? std::string("unreadable") : info.loadWarnings.back());
        }
        s.items.push_back(std::move(info));
    }
}

void TitleLibrary::refresh(LibraryKind kind) {
    KindState& s = state[kind];
    if (!s.builtinsIndexed) {
        indexBuiltins(kind, s);
        s.dirty = true;
    }
    const std::string projectBefore = s.project.signature, userBefore = s.user.signature;
    const bool projectScanned = s.project.scanned, userScanned = s.user.scanned;
    scanScope(kind, LibraryScope::Project, s.project);
    scanScope(kind, LibraryScope::User, s.user);
    if (!projectScanned || !userScanned || projectBefore != s.project.signature || userBefore != s.user.signature) s.dirty = true;
    if (!s.dirty) return;
    s.all.clear();
    s.all.insert(s.all.end(), s.builtins.begin(), s.builtins.end());
    s.all.insert(s.all.end(), s.project.items.begin(), s.project.items.end());
    s.all.insert(s.all.end(), s.user.items.begin(), s.user.items.end());
    s.dirty = false;
    changes++;
}

const std::vector<LibraryItemInfo>& TitleLibrary::items(LibraryKind kind) {
    refresh(kind);
    return state[kind].all;
}

const LibraryItemInfo* TitleLibrary::info(LibraryKind kind, std::string_view refView, LibraryError* error) {
    const std::vector<LibraryItemInfo>& all = items(kind);
    std::string ref(refView);
    for (const LibraryItemInfo& item : all) {
        if (item.ref == ref) return &item;
    }
    // a renamed built-in keeps its old id as an alias (R4)
    const Json& m = manifest();
    if (m.contains("aliases") && m["aliases"].is_object() && m["aliases"].contains(ref) && m["aliases"][ref].is_string()) {
        const std::string target = m["aliases"][ref].get<std::string>();
        for (const LibraryItemInfo& item : all) {
            if (item.ref == target) {
                if (error) {
                    error->deprecated = true;
                    error->resolvedRef = target;
                    error->code = "deprecated";
                    error->message = "'" + ref + "' was renamed '" + target + "'";
                }
                return &item;
            }
        }
    }
    if (error) {
        error->code = "library_item_not_found";
        const std::string q = search::normalize(ref);
        std::vector<std::pair<int, std::string>> scored;
        for (const LibraryItemInfo& item : all) {
            int score = search::fuzzyScore(q, search::normalize(item.ref));
            score = std::max(score, search::fuzzyScore(q, search::normalize(item.name.en)));
            const size_t slash = ref.find('/');
            if (slash != std::string::npos) score = std::max(score, search::fuzzyScore(search::normalize(ref.substr(slash + 1)), search::normalize(item.ref)));
            if (score >= 0) scored.push_back({score, item.ref});
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        error->suggestions.clear();
        for (size_t i = 0; i < scored.size() && error->suggestions.size() < 3; i++) error->suggestions.push_back(scored[i].second);
        std::string words = ref;
        std::replace(words.begin(), words.end(), '/', ' ');
        std::replace(words.begin(), words.end(), '-', ' ');
        error->next = std::string("list_title_library {kind:\"") + libraryKindName(kind) + "\", query:\"" + words + "\"}";
        std::string list;
        for (const std::string& s : error->suggestions) list += (list.empty() ? "" : ", ") + s;
        error->message = std::string("no ") + libraryKindLabel(kind) + " '" + ref + "'" + (list.empty() ? "" : " (closest: " + list + ")") + "; " + error->next +
                         " finds more";
    }
    return nullptr;
}

std::string TitleLibrary::itemText(const LibraryItemInfo& item) {
    if (item.scope == LibraryScope::BuiltIn) {
        auto it = builtinSource().find(item.builtinPath);
        return it == builtinSource().end() ? std::string() : it->second;
    }
    std::string text;
    readText(item.file, text);
    return text;
}

Json TitleLibrary::itemJson(const LibraryItemInfo& item) {
    Json j = Json::parse(itemText(item), nullptr, false);
    return j.is_discarded() ? Json::object() : j;
}

std::shared_ptr<const TitleLook> TitleLibrary::look(std::string_view ref, LibraryError* error) {
    const LibraryItemInfo* item = info(LibraryKind::Look, ref, error);
    if (!item) return nullptr;
    const std::string key = item->ref + "\n" + std::to_string(item->revision) + "\n" + item->file.string() + "\n" + item->stamp;
    auto it = lookCache.find(key);
    if (it != lookCache.end()) return it->second;
    LookParseResult parsed = parseLook(itemText(*item));
    if (!parsed.ok) {
        if (error) {
            error->code = "library_item_invalid";
            error->message = "'" + item->ref + "' does not load: " + parsed.error;
        }
        return nullptr;
    }
    auto shared = std::make_shared<const TitleLook>(std::move(parsed.look));
    lookCache[key] = shared;
    return shared;
}

std::shared_ptr<const titleart::ColorScheme> TitleLibrary::colorScheme(std::string_view ref, LibraryError* error) {
    const LibraryItemInfo* item = info(LibraryKind::ColorScheme, ref, error);
    if (!item) return nullptr;
    const std::string key = item->ref + "\n" + std::to_string(item->revision) + "\n" + item->file.string() + "\n" + item->stamp;
    auto it = schemeCache.find(key);
    if (it != schemeCache.end()) return it->second;
    titleart::ColorScheme scheme;
    std::vector<std::string> warnings;
    if (!titleart::parseColorScheme(itemJson(*item), scheme, &warnings)) {
        if (error) {
            error->code = "library_item_invalid";
            error->message = "'" + item->ref + "' does not load" + (warnings.empty() ? std::string() : ": " + warnings.back());
        }
        return nullptr;
    }
    // the reference is the id a document records (project/<slug> for a project scheme)
    scheme.id = item->ref;
    auto shared = std::make_shared<const titleart::ColorScheme>(std::move(scheme));
    schemeCache[key] = shared;
    return shared;
}

std::shared_ptr<const TitleTemplate> TitleLibrary::templateDoc(std::string_view ref, LibraryError* error) {
    const LibraryItemInfo* item = info(LibraryKind::Template, ref, error);
    if (!item) return nullptr;
    const std::string key = item->ref + "\n" + std::to_string(item->revision) + "\n" + item->file.string() + "\n" + item->stamp;
    auto it = templateCache.find(key);
    if (it != templateCache.end()) return it->second;
    TemplateParseResult parsed = parseTemplate(itemText(*item));
    if (!parsed.ok) {
        if (error) {
            error->code = "library_item_invalid";
            error->message = "'" + item->ref + "' does not load: " + parsed.error;
        }
        return nullptr;
    }
    auto shared = std::make_shared<const TitleTemplate>(std::move(parsed.tmpl));
    templateCache[key] = shared;
    return shared;
}
