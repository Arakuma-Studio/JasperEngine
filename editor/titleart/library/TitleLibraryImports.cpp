// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibraryImports.h"

#include "TitleLibrary.h"
#include "asset/Guid.h"
#include "util/AssetDatabase.h"
#include "asset/AssetMeta.h"
#include "util/Sha256.h"

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
namespace fs = std::filesystem;

namespace {

    bool readBytes(const fs::path& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    // the SHA-256 of a file by path, size and modification time: an apply compares a pack file with
    // the project's copy on every call, and hashing a 2.5 MB font each time made the op's cost the
    // font's size (and the process's memory state) rather than the look's
    bool fileSha(const fs::path& path, std::string& out) {
        struct Stamp {
            uintmax_t size = 0;
            fs::file_time_type time;
            std::string sha;
        };
        static std::mutex lock;
        static std::map<std::string, Stamp> cache;
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        if (ec) return false;
        const fs::file_time_type time = fs::last_write_time(path, ec);
        if (ec) return false;
        const std::string key = path.lexically_normal().string();
        {
            std::lock_guard<std::mutex> guard(lock);
            auto it = cache.find(key);
            if (it != cache.end() && it->second.size == size && it->second.time == time) {
                out = it->second.sha;
                return true;
            }
        }
        std::string bytes;
        if (!readBytes(path, bytes)) return false;
        out = sha256Hex(bytes);
        std::lock_guard<std::mutex> guard(lock);
        cache[key] = Stamp{size, time, out};
        return true;
    }

    struct PackCache {
        fs::path dir;
        std::vector<PackFont> fonts;
        std::vector<PackEnvironment> environments;
        bool loaded = false;
    };

    PackCache& pack() {
        static PackCache cache;
        const fs::path dir = TitleLibrary::packDir();
        if (cache.loaded && cache.dir == dir) return cache;
        cache = PackCache();
        cache.dir = dir;
        cache.loaded = true;
        std::string text;
        if (readBytes(dir / "fonts" / "fonts.json", text)) {
            const Json manifest = Json::parse(text, nullptr, false);
            if (manifest.is_object() && manifest.contains("fonts") && manifest["fonts"].is_object()) {
                for (auto it = manifest["fonts"].begin(); it != manifest["fonts"].end(); ++it) {
                    const Json& e = it.value();
                    PackFont f;
                    f.id = it.key();
                    f.family = e.value("family", std::string());
                    f.style = e.value("style", std::string());
                    if (e.contains("files")) for (const Json& file : e["files"]) if (file.is_string()) f.files.push_back(file.get<std::string>());
                    if (e.contains("variations") && e["variations"].is_object()) f.variations = e["variations"];
                    if (e.contains("scripts")) for (const Json& s : e["scripts"]) if (s.is_string()) f.scripts.push_back(s.get<std::string>());
                    f.category = e.value("category", std::string());
                    f.url = e.value("url", std::string());
                    f.fontVersion = e.value("font_version", std::string());
                    cache.fonts.push_back(std::move(f));
                }
            }
        }
        text.clear();
        if (readBytes(dir / "hdri" / "manifest.json", text)) {
            const Json manifest = Json::parse(text, nullptr, false);
            if (manifest.is_object() && manifest.contains("environments") && manifest["environments"].is_object()) {
                for (auto it = manifest["environments"].begin(); it != manifest["environments"].end(); ++it) {
                    const Json& e = it.value();
                    cache.environments.push_back({it.key(), e.value("file", std::string()), e.value("slug", std::string()),
                                                  e.value("url", std::string()), e.value("title", std::string())});
                }
            }
        }
        return cache;
    }

    std::string quoted(const std::string& text) {
        std::string out = "\"";
        for (char c : text) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + "\"";
    }

    std::string newMeta(const std::string& provenanceYaml, const std::string& importYaml) {
        AssetMeta meta;
        meta.guid = Guid::parse(AssetDatabase::generateGuid());
        if (!importYaml.empty()) meta.setSection("import", importYaml);
        meta.setSection("provenance", provenanceYaml);
        return meta.serialize();
    }

}

bool PackFont::coversJapanese() const {
    return std::find(scripts.begin(), scripts.end(), "ja") != scripts.end();
}

const std::vector<PackFont>& titlelibrary::packFonts() {
    return pack().fonts;
}

const PackFont* titlelibrary::findPackFont(const std::string& id) {
    for (const PackFont& f : pack().fonts) if (f.id == id) return &f;
    return nullptr;
}

const PackEnvironment* titlelibrary::findPackEnvironment(const std::string& id) {
    for (const PackEnvironment& e : pack().environments) if (e.id == id) return &e;
    return nullptr;
}

std::string ImportPlan::copyInto(const std::string& assetDir, const fs::path& source, const std::string& name,
                                 const std::vector<fs::path>& extras, const std::string& provenanceYaml, const std::string& importYaml,
                                 const char* conflictCode) {
    std::error_code sourceEc;
    if (!fs::is_regular_file(source, sourceEc)) return {};
    if (preview || projectDir.empty()) {
        const std::string assetPath = assetDir + "/" + name;
        transient[assetPath] = source.string();
        return assetPath;
    }
    std::string sha;
    if (!fileSha(source, sha)) return {};
    // the canonical folder, then numbered ones after a conflict (<dir>-2, -3 ...)
    for (int n = 1; n < 100; n++) {
        const std::string dir = n == 1 ? assetDir : assetDir + "-" + std::to_string(n);
        const std::string assetPath = dir + "/" + name;
        auto plannedIt = planned.find(assetPath);
        if (plannedIt != planned.end()) {
            if (plannedIt->second == sha) return assetPath;
            continue;
        }
        const fs::path target = assetsDir / fs::path(assetPath);
        std::error_code ec;
        if (fs::exists(target, ec)) {
            std::string existing;
            if (fileSha(target, existing) && existing == sha) {
                planned[assetPath] = sha;
                return assetPath;      // already imported: nothing to copy
            }
            if (n == 1) {
                warnings.push_back(Json{{"code", conflictCode},
                                        {"message", "'" + assetPath + "' exists with other bytes: the library copy goes to " + assetDir + "-2/"}});
            }
            continue;
        }
        std::string bytes;
        if (!readBytes(source, bytes)) return {};
        planned[assetPath] = sha;
        files.push_back({target, bytes, true, false});
        files.push_back({fs::path(target.string() + ".meta"), newMeta(provenanceYaml, importYaml), true, false});
        for (const fs::path& extra : extras) {
            std::string extraBytes;
            const fs::path extraTarget = assetsDir / fs::path(dir) / extra.filename();
            if (fs::exists(extraTarget, ec) || !readBytes(extra, extraBytes)) continue;
            files.push_back({extraTarget, extraBytes, true, false});
        }
        imported.push_back(assetPath);
        return assetPath;
    }
    return {};
}

std::string ImportPlan::font(const std::string& packId, const std::string& file) {
    const PackFont* f = findPackFont(packId);
    if (!f || f->files.empty()) {
        warnings.push_back(Json{{"code", "font_unavailable"},
                                {"message", "the library font '" + packId + "' is not installed (no staged pack at " + TitleLibrary::packDir().string() +
                                                "): the layer keeps its font"},
                                {"fix", "reinstall the editor, or set JASPERENGINE_TITLEART_LIBRARY_DIR to a folder holding fonts/"}});
        return {};
    }
    std::string name = f->files.front();
    if (!file.empty() && std::find(f->files.begin(), f->files.end(), file) != f->files.end()) name = file;
    const fs::path dir = TitleLibrary::packDir() / "fonts" / packId;
    std::string sha;
    fileSha(dir / name, sha);
    const std::string provenance = "{version: 1, licence: OFL-1.1, source: titleart-pack, url: " + quoted(f->url) +
                                   ", builtin: {set: fonts, id: " + quoted(packId) + ", sha256: " + sha + "}}";
    const std::string path = copyInto(std::string(TITLE_FONTS_ASSET_DIR) + "/" + packId, dir / name, name, {dir / "OFL.txt"}, provenance, {}, "font_conflict");
    if (path.empty()) {
        warnings.push_back(Json{{"code", "font_unavailable"}, {"message", "the library font file '" + (dir / name).string() + "' cannot be read"}});
    }
    return path;
}

std::string ImportPlan::environment(const std::string& envId) {
    const PackEnvironment* e = findPackEnvironment(envId);
    if (!e) {
        warnings.push_back(Json{{"code", "environment_unavailable"},
                                {"message", "the library environment '" + envId + "' is not installed: the reflection map is left empty"},
                                {"fix", "reinstall the editor, or set JASPERENGINE_TITLEART_LIBRARY_DIR to a folder holding hdri/"}});
        return {};
    }
    const std::string provenance = "{version: 1, licence: CC0-1.0, source: polyhaven, url: " + quoted(e->url) + "}";
    const std::string import = "{textureImporter: {version: 1, type: hdr}}";
    return copyInto(TITLE_ENVIRONMENTS_ASSET_DIR, TitleLibrary::packDir() / "hdri" / e->file, e->file, {}, provenance, import, "environment_conflict");
}

void ImportPlan::ensure(const std::string& assetPath) {
    // TitleArt/Fonts/<pack id>/<file> or TitleArt/Environments/<file>
    const std::string fonts = std::string(TITLE_FONTS_ASSET_DIR) + "/";
    const std::string envs = std::string(TITLE_ENVIRONMENTS_ASSET_DIR) + "/";
    if (assetPath.rfind(fonts, 0) == 0) {
        const std::string rest = assetPath.substr(fonts.size());
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return;
        font(rest.substr(0, slash), rest.substr(slash + 1));
    } else if (assetPath.rfind(envs, 0) == 0) {
        const std::string file = assetPath.substr(envs.size());
        for (const PackEnvironment& e : pack().environments) {
            if (e.file == file) {
                environment(e.id);
                return;
            }
        }
    }
}

Json ImportPlan::toJson() const {
    Json out = Json::object();
    out["imported"] = imported;
    out["warnings"] = warnings;
    return out;
}
