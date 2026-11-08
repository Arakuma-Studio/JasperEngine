// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SpriteAtlasBuilder.h"
#include "SpriteSheetBuilder.h"

#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"
#include "pixelart/aseprite/AsepriteImporter.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureImportSettings.h"

#include "asset/ImportedAssets.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "sprite/SpriteFrameGeometry.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"
#include "texture/Texture.h"
#include "util/Sha256.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>

namespace fs = std::filesystem;

namespace jasperengine::editor::sprite {

    namespace {

        using pixelart::IntPoint;
        using pixelart::IntRect;
        using pixelart::IntSize;
        using pixelart::PixelImage;
        using pixelart::PixelProblem;

        constexpr const char* kBuildFormat = "jasperengine.spriteatlas-build";

        std::string fileStamp(const fs::path& path) {
            std::error_code ec;
            const auto time = fs::last_write_time(path, ec);
            if (ec) return "-";
            const uintmax_t size = fs::file_size(path, ec);
            return std::to_string(size) + "@" + std::to_string(time.time_since_epoch().count());
        }

        bool writeCacheText(const fs::path& path, const std::string& text) {
            io::AtomicWriteOptions options;
            options.durability = io::Durability::Cache;
            options.skipIfUnchanged = true;
            return io::writeFileAtomic(path, text, options).ok();
        }

        std::string readText(const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }

        nlohmann::json problemJson(const PixelProblem& p) {
            nlohmann::json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix}};
            if (!p.field.empty()) j["field"] = p.field;
            return j;
        }

        PixelProblem problemFrom(const nlohmann::json& j) {
            return {j.value("kind", std::string()), j.value("field", std::string()), j.value("message", std::string()), j.value("fix", std::string())};
        }

        std::string withoutExtension(const std::string& path) {
            const fs::path p(path);
            return (p.parent_path() / p.stem()).generic_string();
        }

        std::string thousands(long long n) {
            std::string digits = std::to_string(n < 0 ? -n : n);
            std::string out;
            for (size_t i = 0; i < digits.size(); i++) {
                if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
                out += digits[i];
            }
            return (n < 0 ? "-" : "") + out;
        }

        std::string megabytes(size_t bytes) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
            return text;
        }

        bool underFolder(const std::string& path, const std::string& folder) {
            if (folder.empty()) return true;
            std::string f = folder;
            while (!f.empty() && f.back() == '/') f.pop_back();
            return path == f || path.rfind(f + "/", 0) == 0;
        }

        nlohmann::json rectJson(const IntRect& r) { return {r.x, r.y, r.w, r.h}; }

        // what a cache hit reads back
        nlohmann::json buildJson(const AtlasBuildResult& r) {
            nlohmann::json pages = nlohmann::json::array();
            for (const AtlasPageRow& p : r.pages) pages.push_back({{"size", {p.width, p.height}}, {"frames", p.frames}, {"used", p.used}, {"file", p.file}, {"hash", p.hash}});
            nlohmann::json frames = nlohmann::json::array();
            for (const AtlasFrameRow& f : r.frames) {
                frames.push_back({{"name", f.name}, {"member", f.member}, {"member_frame", f.memberFrame}, {"page", f.page}, {"rect", rectJson(f.rect)},
                                  {"source_size", {f.sourceSize.w, f.sourceSize.h}}, {"offset", {f.offset.x, f.offset.y}}, {"waste", f.waste},
                                  {"duplicate_of", f.duplicateOf}});
            }
            nlohmann::json warnings = nlohmann::json::array();
            for (const PixelProblem& w : r.warnings) warnings.push_back(problemJson(w));
            nlohmann::json worst = nlohmann::json::array();
            for (const auto& [member, waste] : r.worstMembers) worst.push_back({member, waste});
            return {{"format", kBuildFormat}, {"version", 1}, {"key", r.key}, {"atlas", r.atlasPath}, {"guid", r.guid.str()}, {"platform", r.platform},
                    {"compression", r.compression}, {"filter", r.filter}, {"members", r.members}, {"member_files", r.memberFiles},
                    {"member_pages", r.memberPages}, {"pages", pages}, {"frames", frames}, {"duplicates", r.duplicates}, {"usage", r.usage},
                    {"warnings", warnings}, {"worst_members", worst}, {"ms", r.ms}, {"web_pages", r.webPages}};
        }

        bool fromBuildJson(const nlohmann::json& j, AtlasBuildResult& r) {
            if (!j.is_object() || j.value("format", std::string()) != kBuildFormat || j.value("version", 0) != 1) return false;
            r.compression = j.value("compression", std::string());
            r.filter = j.value("filter", std::string());
            r.members = j.value("members", std::vector<std::string>());
            r.memberFiles = j.value("member_files", std::map<std::string, std::string>());
            r.memberPages = j.value("member_pages", std::map<std::string, int>());
            for (const auto& p : j.value("pages", nlohmann::json::array())) {
                AtlasPageRow row;
                const auto size = p.value("size", std::vector<int>{0, 0});
                row.width = size.size() > 0 ? size[0] : 0;
                row.height = size.size() > 1 ? size[1] : 0;
                row.frames = p.value("frames", 0);
                row.used = p.value("used", 0.0);
                row.file = p.value("file", std::string());
                row.hash = p.value("hash", std::string());
                r.pages.push_back(row);
            }
            for (const auto& f : j.value("frames", nlohmann::json::array())) {
                AtlasFrameRow row;
                row.name = f.value("name", std::string());
                row.member = f.value("member", std::string());
                row.memberFrame = f.value("member_frame", std::string());
                row.page = f.value("page", 0);
                const auto rect = f.value("rect", std::vector<int>{0, 0, 0, 0});
                if (rect.size() == 4) row.rect = {rect[0], rect[1], rect[2], rect[3]};
                const auto source = f.value("source_size", std::vector<int>{0, 0});
                if (source.size() == 2) row.sourceSize = {source[0], source[1]};
                const auto offset = f.value("offset", std::vector<int>{0, 0});
                if (offset.size() == 2) row.offset = {offset[0], offset[1]};
                row.waste = f.value("waste", 0);
                row.duplicateOf = f.value("duplicate_of", -1);
                r.frames.push_back(row);
            }
            for (const auto& w : j.value("warnings", nlohmann::json::array())) r.warnings.push_back(problemFrom(w));
            for (const auto& w : j.value("worst_members", nlohmann::json::array())) {
                if (w.is_array() && w.size() == 2) r.worstMembers.emplace_back(w[0].get<std::string>(), w[1].get<int>());
            }
            r.duplicates = j.value("duplicates", 0);
            r.usage = j.value("usage", 0.0);
            r.webPages = j.value("web_pages", std::vector<std::pair<int, int>>());
            return true;
        }

        std::string exportDirOf(const AtlasBuildResult& r) {
            return std::string(ImportedAssets::kImportedDir) + "/atlas/" + r.guid.str() + "/" + r.platform + "/";
        }

        bool isImagePath(const std::string& path) { return SpriteSheetSources::isTexturePath(path); }

    }

    const char* atlasModeName(AtlasMode mode) {
        switch (mode) {
            case AtlasMode::EnabledForBuilds: return "enabled_for_builds";
            case AtlasMode::Disabled: return "disabled";
            case AtlasMode::Enabled: break;
        }
        return "enabled";
    }

    AtlasMode atlasModeFromName(const std::string& name) {
        if (name == "enabled_for_builds") return AtlasMode::EnabledForBuilds;
        if (name == "disabled") return AtlasMode::Disabled;
        return AtlasMode::Enabled;
    }

    std::string AtlasBuildInput::key() const {
        std::string text = writeSpriteAtlas(doc) + "|atlas " + atlasPath + "|platform " + platform + "|compression " + compression + "|filter " + filter +
                           "|block " + (forceBlockAlign ? "1" : "0") + "|packer " + std::to_string(kPackerVersion) + "|builder " +
                           std::to_string(kAtlasBuilderVersion) + "|sheet " + std::to_string(SPRITESHEET_VERSION);
        for (const AtlasMemberInput& m : members) text += "|" + m.kind + " " + m.path + " " + m.stamp;
        return sha256Hex(text);
    }

    fs::path AtlasBuildResult::memberFile(const std::string& member) const {
        auto it = memberFiles.find(member);
        return it == memberFiles.end() ? fs::path() : dir / it->second;
    }

    size_t AtlasBuildResult::vramBytes(const std::string& family) const {
        // lossless pages are RGBA8 everywhere; the platforms' sprite formats (BC7, ASTC 4x4, ETC2
        // RGBA, UASTC) are 8 bits a pixel
        size_t pixels = 0;
        if (family == "web" && !webPages.empty()) {
            for (const auto& [w, h] : webPages) pixels += static_cast<size_t>(w) * static_cast<size_t>(h);
        } else {
            for (const AtlasPageRow& p : pages) pixels += static_cast<size_t>(p.width) * static_cast<size_t>(p.height);
        }
        return compression == "lossless" ? pixels * 4 : pixels;
    }

    std::string AtlasBuildResult::statsLine() const {
        const int percent = static_cast<int>(std::lround(usage * 100.0));
        std::string line = std::to_string(pages.size()) + (pages.size() == 1 ? " page" : " pages") + " \xC2\xB7 " + thousands(static_cast<long long>(frames.size())) +
                           " frames \xC2\xB7 " + std::to_string(percent) + " % used";
        if (duplicates > 0) line += " \xC2\xB7 " + thousands(duplicates) + " duplicate frames merged";
        if (platform.empty() || platform == "desktop") {
            // R29: the Desktop and Web families (the web layout when its max size differs)
            line += " \xC2\xB7 " + megabytes(vramBytes("desktop")) + " VRAM (Desktop) \xC2\xB7 " + megabytes(vramBytes("web")) + " (Web)";
        } else {
            std::string family = platform;
            family[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(family[0])));
            line += " \xC2\xB7 " + megabytes(vramBytes(platform)) + " VRAM (" + family + ")";
        }
        return line;
    }

    nlohmann::json AtlasBuildResult::toJson(const std::string& search, int limit, int page, bool withFrames) const {
        nlohmann::json pagesJson = nlohmann::json::array();
        for (size_t i = 0; i < pages.size(); i++) {
            const AtlasPageRow& p = pages[i];
            const size_t pixels = static_cast<size_t>(p.width) * static_cast<size_t>(p.height);
            pagesJson.push_back({{"index", i}, {"size", {p.width, p.height}}, {"frames", p.frames}, {"used_percent", std::lround(p.used * 100.0)},
                                 {"vram_bytes", {{"lossless", pixels * 4}, {"platform", compression == "lossless" ? pixels * 4 : pixels}}},
                                 {"wasted_pixels", static_cast<long long>(pixels) - static_cast<long long>(std::llround(p.used * static_cast<double>(pixels)))}});
        }
        nlohmann::json j = {{"platform", platform}, {"compression", compression}, {"filter", filter}, {"pages", pagesJson},
                            {"frame_count", frames.size()}, {"duplicates", duplicates}, {"usage_percent", std::lround(usage * 100.0)},
                            {"members", members}, {"stats", statsLine()}, {"relayout", relayout}, {"cache_hit", cacheHit},
                            {"vram_bytes", {{"desktop", vramBytes("desktop")}, {"web", vramBytes("web")}}}};
        nlohmann::json worst = nlohmann::json::array();
        for (size_t i = 0; i < worstMembers.size() && i < 5; i++) worst.push_back({{"member", worstMembers[i].first}, {"transparent_pixels", worstMembers[i].second}});
        j["worst_members"] = worst;
        if (!withFrames) return j;
        // search: a name substring, member:<substring>, page:<n> (space separated, all must match)
        std::vector<std::string> terms;
        for (size_t start = 0; start <= search.size();) {
            const size_t space = search.find(' ', start);
            const std::string term = search.substr(start, space == std::string::npos ? std::string::npos : space - start);
            if (!term.empty()) terms.push_back(term);
            if (space == std::string::npos) break;
            start = space + 1;
        }
        nlohmann::json list = nlohmann::json::array();
        int matched = 0;
        for (size_t i = 0; i < frames.size(); i++) {
            const AtlasFrameRow& f = frames[i];
            if (page >= 0 && f.page != page) continue;
            bool ok = true;
            for (const std::string& t : terms) {
                if (t.rfind("member:", 0) == 0) ok = ok && f.member.find(t.substr(7)) != std::string::npos;
                else if (t.rfind("page:", 0) == 0) ok = ok && std::to_string(f.page) == t.substr(5);
                else ok = ok && f.name.find(t) != std::string::npos;
            }
            if (!ok) continue;
            matched++;
            if (limit > 0 && static_cast<int>(list.size()) >= limit) continue;
            nlohmann::json row = {{"index", i}, {"name", f.name}, {"member", f.member}, {"page", f.page}, {"rect", rectJson(f.rect)},
                                  {"trim", {{"source_size", {f.sourceSize.w, f.sourceSize.h}}, {"offset", {f.offset.x, f.offset.y}}}}, {"waste", f.waste}};
            if (f.duplicateOf >= 0) row["duplicate_of"] = frames[static_cast<size_t>(f.duplicateOf)].name;
            list.push_back(row);
        }
        j["frames"] = list;
        j["frames_matched"] = matched;
        return j;
    }

    std::vector<AtlasBuildResult::ExportedFile> AtlasBuildResult::exportedFiles(bool remaps) const {
        std::vector<ExportedFile> out;
        if (!ok) return out;
        const std::string base = exportDirOf(*this);
        out.push_back({atlasPath, "sheet", sheetFile(), base + "sheet.json"});
        for (size_t p = 0; p < pages.size(); p++) out.push_back({atlasPath, "image#" + std::to_string(p), pageFile(p), base + pages[p].file});
        if (!remaps) return out;
        for (const auto& [member, file] : memberFiles) {
            out.push_back({member, "sheet", dir / file, base + file});
            out.push_back({member, "atlas", sheetFile(), base + "sheet.json"});
            for (size_t p = 0; p < pages.size(); p++) out.push_back({member, "image#" + std::to_string(p), pageFile(p), base + pages[p].file});
        }
        return out;
    }

    // ------------------------------------------------------------------ builder

    SpriteAtlasBuilder& SpriteAtlasBuilder::get() {
        static SpriteAtlasBuilder* builder = new SpriteAtlasBuilder();
        return *builder;
    }

    fs::path SpriteAtlasBuilder::cacheRootOf(const Project& project) {
        return project.getProjectInternalPath() / "cache" / "atlas" / "v1";
    }

    fs::path SpriteAtlasBuilder::cacheRoot() const {
        return project ? cacheRootOf(*project) : fs::path();
    }

    std::string SpriteAtlasBuilder::assetPathOf(const std::string& path) const {
        if (!project) return std::string();
        fs::path p(path);
        if (!p.is_absolute()) p = project->getAssetsPath() / p;
        const std::string text = p.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
        if (text.empty() || text == "." || text.rfind("..", 0) == 0) return std::string();
        return text;
    }

    std::optional<SpriteAtlasDocument> SpriteAtlasBuilder::readDocument(const fs::path& file, PixelProblem* problem) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            if (problem) *problem = {"missing_member", "path", "no file " + file.generic_string(), "check the path (list_assets finds .spriteatlas files)"};
            return std::nullopt;
        }
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SpriteAtlasParseResult parsed = parseSpriteAtlas(text);
        if (!parsed.ok()) {
            if (problem) *problem = parsed.error;
            return std::nullopt;
        }
        return std::move(*parsed.document);
    }

    void SpriteAtlasBuilder::attach(Project* newProject) {
        if (newProject == project) return;
        if (project) {
            for (auto& [path, r] : running) {
                r.cancel->cancel();
                r.task.wait();
            }
            running.clear();
            again.clear();
            waitingImports.clear();
            ImportedAssets::setSheetProvider(nullptr);
            SetAssetMetaCmd::removeWriteListener(this);
            pixelart::aseprite::AsepriteImporter::get().removeImportListener(this);
            std::lock_guard<std::mutex> lock(mutex);
            results.clear();
            remaps.clear();
            packers.clear();
            memberCache.clear();
            builds.clear();
        }
        project = newProject;
        SpriteSheetBuilder::get().attach(project);
        if (!project) return;
        currentMode = atlasModeFromName(project->getSpriteAtlasMode());
        ImportedAssets::setSheetProvider([this](const std::string& ref, std::string& out) { return resolve(ref, out); });
        // a texture's Sprite Mode or import settings (and their undo): its sheet and the atlases packing it
        SetAssetMetaCmd::addWriteListener(this, [this](Project* writer, const fs::path& asset, const std::string& key) {
            if (writer != project || key.rfind("import", 0) != 0) return;
            const std::string assetPath = assetPathOf((project->getProjectPath() / asset).string());
            if (assetPath.empty() || !isImagePath(assetPath)) return;
            SpriteSheetBuilder::get().invalidate(assetPath);
            memberChanged(assetPath);
        });
        pixelart::aseprite::AsepriteImporter::get().addImportListener(this, [this](const std::string& assetPath) { memberChanged(assetPath); });
        requestAll();
    }

    void SpriteAtlasBuilder::forget(const std::string& path, bool removeCache) {
        const std::string atlasPath = assetPathOf(path);
        auto it = running.find(atlasPath);
        if (it != running.end()) it->second.cancel->cancel();
        again.erase(atlasPath);
        waitingImports.erase(atlasPath);
        fs::path folder;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto r = results.find(atlasPath);
            if (r != results.end() && !r->second->dir.empty()) folder = r->second->dir.parent_path();
            results.erase(atlasPath);
            attempts.erase(atlasPath);
        }
        refreshRemaps();
        SpriteSheetPool::invalidate(atlasPath);
        // a deleted atlas leaves no cache behind (a renamed one keeps its GUID: the next build
        // writes the folder again)
        if (removeCache && !folder.empty() && it == running.end()) {
            std::error_code ec;
            fs::remove_all(folder, ec);
        }
    }

    void SpriteAtlasBuilder::pruneCache() {
        if (!project) return;
        // the folders of the project's atlases; in each, the editor's desktop build and the exports'
        std::set<std::string> folders;
        for (const std::string& atlas : atlasPaths()) {
            std::optional<SpriteAtlasDocument> doc = readDocument(project->getAssetsPath() / atlas, nullptr);
            if (doc) folders.insert(doc->guid.isNull() ? sha256Hex(atlas).substr(0, 32) : doc->guid.str());
        }
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(cacheRoot(), ec)) {
            if (!entry.is_directory(ec)) continue;
            if (!folders.count(entry.path().filename().string())) {
                fs::remove_all(entry.path(), ec);
                continue;
            }
            for (const fs::directory_entry& family : fs::directory_iterator(entry.path(), ec)) {
                const std::string name = family.path().filename().string();
                if (family.is_directory(ec) && name != "desktop" && name.rfind("export-", 0) != 0) fs::remove_all(family.path(), ec);
            }
        }
    }

    AtlasMode SpriteAtlasBuilder::mode() const {
        return currentMode;
    }

    void SpriteAtlasBuilder::modeChanged() {
        if (!project) return;
        currentMode = atlasModeFromName(project->getSpriteAtlasMode());
        if (currentMode == AtlasMode::Disabled) {
            cancelAll();
            refreshRemaps();
            return;
        }
        refreshRemaps();
        requestAll();
    }

    std::vector<std::string> SpriteAtlasBuilder::atlasPaths() const {
        std::vector<std::string> paths;
        if (!project) return paths;
        project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
            if (info.type != AssetType::SpriteAtlas) return;
            const std::string assetPath = assetPathOf((project->getProjectPath() / info.path).string());
            if (!assetPath.empty()) paths.push_back(assetPath);
        });
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    void SpriteAtlasBuilder::requestAll() {
        if (!project) return;
        if (running.empty()) pruneCache();
        if (currentMode == AtlasMode::Disabled) return;
        for (const std::string& path : atlasPaths()) rebuild(path);
    }

    void SpriteAtlasBuilder::rebuild(const std::string& path) {
        const std::string atlasPath = assetPathOf(path);
        if (atlasPath.empty() || !project) return;
        if (running.count(atlasPath)) {
            again.insert(atlasPath);
            return;
        }
        start(atlasPath);
    }

    void SpriteAtlasBuilder::memberChanged(const std::string& memberPath) {
        const std::string member = assetPathOf(memberPath);
        if (member.empty()) return;
        std::vector<std::string> atlases;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& [atlas, r] : results) {
                if (std::find(r->members.begin(), r->members.end(), member) != r->members.end()) atlases.push_back(atlas);
            }
        }
        // the folders of every atlas (a file added to a folder member joins it)
        for (const std::string& atlas : atlasPaths()) {
            if (std::find(atlases.begin(), atlases.end(), atlas) != atlases.end()) continue;
            std::optional<SpriteAtlasDocument> doc = readDocument(project->getAssetsPath() / atlas, nullptr);
            if (!doc) continue;
            for (const SpriteAtlasMember& m : doc->members) {
                if ((m.isFolder() && underFolder(member, m.folder)) || (!m.isFolder() && m.path == member)) {
                    atlases.push_back(atlas);
                    break;
                }
            }
        }
        for (const std::string& atlas : atlases) rebuild(atlas);
    }

    bool SpriteAtlasBuilder::gather(Project& project, const std::string& atlasPath, const SpriteAtlasDocument& doc, const std::string& platform,
                                    AtlasBuildInput& out) {
        out = AtlasBuildInput();
        out.atlasPath = atlasPath;
        out.doc = doc;
        out.platform = platform.empty() ? std::string("desktop") : platform;
        const fs::path assets = project.getAssetsPath().lexically_normal();
        auto assetRelative = [&](const fs::path& projectRelative) {
            return (project.getProjectPath() / projectRelative).lexically_normal().lexically_relative(assets).generic_string();
        };
        std::map<std::string, std::string> found;   // asset path -> kind
        auto kindOf = [](AssetType type) -> std::string {
            if (type == AssetType::Texture) return "texture";
            if (type == AssetType::AsepriteSprite) return "aseprite_sprite";
            return std::string();
        };
        for (const SpriteAtlasMember& m : doc.members) {
            if (!m.isFolder()) {
                std::string path = m.path;
                if (!m.guid.isNull()) {
                    AssetDatabase::AssetInfo info;
                    if (project.getAssetDatabase().getInfo(m.guid, info)) path = assetRelative(info.path);
                }
                std::error_code ec;
                const fs::path absolute = assets / path;
                const bool exists = fs::is_regular_file(absolute, ec);
                std::string kind = SpriteSheetPool::isAsepritePath(path) ? "aseprite_sprite" : isImagePath(path) ? "texture" : std::string();
                if (!exists || kind.empty()) {
                    out.problems.push_back({"missing_member", m.path, exists ? m.path + " is not a texture or an .aseprite file" : "the member " + m.path + " is missing",
                                            exists ? "remove it from the atlas" : "restore the file, or remove the member (edit_sprite_atlas remove)"});
                    continue;
                }
                found[path] = kind;
                continue;
            }
            std::error_code ec;
            if (!fs::is_directory(assets / m.folder, ec)) {
                out.problems.push_back({"missing_member", m.folder, "the folder member " + m.folder + " is missing (a folder has no GUID: a moved one is not followed)",
                                        "add the folder at its new path and remove the old one"});
                continue;
            }
            project.getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
                const std::string kind = kindOf(info.type);
                if (kind.empty()) return;
                if (!m.types.empty() && std::find(m.types.begin(), m.types.end(), kind) == m.types.end()) return;
                const std::string path = assetRelative(info.path);
                if (path.rfind("..", 0) == 0 || !underFolder(path, m.folder)) return;
                found[path] = kind;
            });
        }
        pixelart::aseprite::AsepriteImporter& importer = pixelart::aseprite::AsepriteImporter::get();
        for (const auto& [path, kind] : found) {
            bool excluded = false;
            for (const std::string& e : doc.exclude) excluded = excluded || underFolder(path, e);
            if (excluded) continue;
            AtlasMemberInput member;
            member.path = path;
            member.absolute = assets / path;
            member.kind = kind;
            if (kind == "aseprite_sprite") {
                std::shared_ptr<const pixelart::aseprite::ImportRecord> record = importer.record(path);
                if (importer.isPending(path) || !record || !record->ok) {
                    if (!importer.isPending(path) && !record) importer.reimport(path);
                    out.problems.push_back({importer.isPending(path) || !record ? "importing" : "missing_member", path,
                                            record && !record->ok ? path + " does not import: " + record->error.message : path + " is importing",
                                            record && !record->ok ? "fix the file's import warnings" : "the atlas builds once it is imported"});
                    continue;
                }
                member.stamp = record->key;
                member.sheetFile = record->sheetFile();
                for (size_t p = 0; p < record->pages.size(); p++) member.pageFiles.push_back(record->pageFile(p));
                member.pixelArt = true;
            } else {
                member.stamp = fileStamp(member.absolute) + "|" + fileStamp(SetAssetMetaCmd::metaPathOf(member.absolute));
                const TextureSheetInfo sheetInfo = textureSheetSettings(member.absolute);
                TextureImportSettings settings;
                const bool stored = TextureImportSettings::load(member.absolute.string(), settings);
                member.pixelArt = sheetInfo.isSheet() || (stored && settings.quality && *settings.quality == TextureCookQuality::None);
            }
            out.members.push_back(std::move(member));
        }
        bool pixelArt = false;
        for (const AtlasMemberInput& m : out.members) pixelArt = pixelArt || m.pixelArt;
        const std::string& compression = doc.settings.compression;
        out.compression = compression == "lossless" || (compression == "auto" && pixelArt) ? "lossless" : "platform_default";
        const std::string& filter = doc.settings.filter;
        out.filter = filter == "point" || (filter == "auto" && pixelArt) ? "point" : "bilinear";
        // a power-of-two texture strategy resizes what is not one: the pages are made power-of-two
        // instead, so nothing resamples them
        if (textureProjectSettingsOf(project).strategy != TextureSizeStrategy::None) out.doc.settings.powerOfTwo = true;
        const std::string folder = doc.guid.isNull() ? sha256Hex(atlasPath).substr(0, 32) : doc.guid.str();
        out.outDir = cacheRootOf(project) / folder / out.platform;
        return true;
    }

    std::shared_ptr<const AtlasMemberFrames> SpriteAtlasBuilder::memberFrames(const AtlasMemberInput& member) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = memberCache.find(member.path);
            if (it != memberCache.end() && it->second->stamp == member.stamp && it->second->kind == member.kind) return it->second;
        }
        auto frames = std::make_shared<AtlasMemberFrames>();
        frames->path = member.path;
        frames->kind = member.kind;
        frames->stamp = member.stamp;
        if (member.kind == "aseprite_sprite") {
            SpriteSheetParseResult parsed = parseSheet(readText(member.sheetFile));
            if (!parsed.ok()) {
                frames->error = {"missing_member", member.path, member.path + ": its import does not read (" + parsed.error + ")", "reimport it"};
            } else {
                frames->sheet = std::move(*parsed.sheet);
                std::vector<std::optional<PixelImage>> pages;
                for (const fs::path& file : member.pageFiles) pages.push_back(loadSpriteImage(file, nullptr));
                for (const SpriteFrame& f : frames->sheet.frames) {
                    auto image = std::make_shared<PixelImage>(static_cast<int>(f.sourceSize.x), static_cast<int>(f.sourceSize.y));
                    if (f.page < pages.size() && pages[f.page] && f.rect.getWidth() > 0 && f.rect.getHeight() > 0) {
                        Rect r = f.rect;
                        const PixelImage cut = pages[f.page]->crop({static_cast<int>(r.getX()), static_cast<int>(r.getY()), static_cast<int>(r.getWidth()), static_cast<int>(r.getHeight())});
                        image->blit(cut, static_cast<int>(f.offset.x), static_cast<int>(f.offset.y));
                    }
                    frames->images.push_back(std::move(image));
                }
                // the frames sit in the source frame again: no trim of the import's kept
                for (SpriteFrame& f : frames->sheet.frames) {
                    f.offset = Vector2(0.0f, 0.0f);
                    f.rect = Rect(0.0f, 0.0f, f.sourceSize.x, f.sourceSize.y);
                }
            }
        } else {
            std::string error;
            std::optional<PixelImage> image = loadSpriteImage(member.absolute, &error);
            if (!image) {
                frames->error = {"missing_member", member.path, member.path + ": " + error, "save it as an image this editor reads"};
            } else {
                const TextureSheetInfo info = textureSheetSettings(member.absolute);
                TextureSheetBuild built = buildTextureSheet(member.absolute, member.path, info.settings, &*image);
                frames->sheet = std::move(built.sheet);
                for (const SpriteFrame& f : frames->sheet.frames) {
                    // a trimmed frame (an Aseprite --trim export) goes back into its source frame
                    Rect r = f.rect;
                    const PixelImage cut = image->crop({static_cast<int>(r.getX()), static_cast<int>(r.getY()), static_cast<int>(r.getWidth()), static_cast<int>(r.getHeight())});
                    const int sw = std::max(static_cast<int>(f.sourceSize.x), cut.width()), sh = std::max(static_cast<int>(f.sourceSize.y), cut.height());
                    if (sw == cut.width() && sh == cut.height()) {
                        frames->images.push_back(std::make_shared<PixelImage>(cut));
                        continue;
                    }
                    auto source = std::make_shared<PixelImage>(sw, sh);
                    source->blit(cut, static_cast<int>(f.offset.x), static_cast<int>(f.offset.y));
                    frames->images.push_back(std::move(source));
                }
                for (SpriteFrame& f : frames->sheet.frames) {
                    f.sourceSize = Vector2(static_cast<float>(std::max(static_cast<int>(f.sourceSize.x), static_cast<int>(f.rect.getWidth()))),
                                           static_cast<float>(std::max(static_cast<int>(f.sourceSize.y), static_cast<int>(f.rect.getHeight()))));
                    f.offset = Vector2(0.0f, 0.0f);
                    f.rect = Rect(0.0f, 0.0f, f.sourceSize.x, f.sourceSize.y);
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        memberCache[member.path] = frames;
        return frames;
    }

    std::shared_ptr<AtlasBuildResult> SpriteAtlasBuilder::build(const AtlasBuildInput& input, AtlasProgress* progress, const jobs::CancelToken* cancel) {
        const auto t0 = std::chrono::steady_clock::now();
        auto result = std::make_shared<AtlasBuildResult>();
        result->atlasPath = input.atlasPath;
        result->platform = input.platform;
        result->guid = input.doc.guid;
        result->compression = input.compression;
        result->filter = input.filter;
        result->dir = input.outDir;
        result->key = input.key();
        auto elapsed = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
        auto cancelled = [&]() { return cancel && cancel->isCancelled(); };
        for (const PixelProblem& p : input.problems) result->warnings.push_back(p);

        // a build of the same inputs is on disk: adopted as it is
        if (!input.outDir.empty()) {
            const nlohmann::json j = nlohmann::json::parse(readText(input.outDir / "build.json"), nullptr, false);
            AtlasBuildResult cached = *result;
            cached.warnings.clear();
            if (!j.is_discarded() && j.value("key", std::string()) == result->key && fromBuildJson(j, cached)) {
                SpriteSheetParseResult sheet = parseSheet(readText(cached.sheetFile()));
                bool complete = sheet.ok();
                for (size_t p = 0; complete && p < cached.pages.size(); p++) complete = fs::exists(cached.pageFile(p));
                if (complete) {
                    *result = std::move(cached);
                    result->sheet = std::move(*sheet.sheet);
                    result->ok = true;
                    result->cacheHit = true;
                    result->relayout = false;
                    result->ms = elapsed();
                    return result;
                }
            }
        }

        // members: read once per content (the cache), packed by path
        std::vector<std::shared_ptr<const AtlasMemberFrames>> members;
        for (const AtlasMemberInput& m : input.members) {
            if (cancelled()) break;
            std::shared_ptr<const AtlasMemberFrames> frames = memberFrames(m);
            if (!frames->error.empty()) {
                result->warnings.push_back(frames->error);
                continue;
            }
            if (frames->sheet.frames.empty()) {
                result->warnings.push_back({"missing_member", m.path, m.path + " has no frames", "set its Sprite Mode, or remove it"});
                continue;
            }
            members.push_back(std::move(frames));
        }
        if (cancelled()) {
            result->error = {"cancelled", "", "the build was cancelled", "build it again"};
            return result;
        }

        PackSettings settings;
        settings.padding = input.doc.settings.padding;
        settings.extrude = input.doc.settings.extrude;
        // linear filtering reads a texel past each edge (and one more when scaled down)
        if (input.filter == "bilinear" && settings.extrude < 2) settings.extrude = 2;
        settings.maxSize = input.doc.settings.maxSizeFor(input.platform);
        settings.trim = input.doc.settings.trim;
        settings.keepMembersTogether = input.doc.settings.keepMembersTogether;
        settings.powerOfTwo = input.doc.settings.powerOfTwo;
        settings.square = input.doc.settings.square;
        settings.blockAlign = input.compression == "platform_default" || input.forceBlockAlign;

        std::vector<PackMember> packMembers;
        for (const auto& m : members) {
            PackMember pm;
            pm.path = m->path;
            for (size_t i = 0; i < m->sheet.frames.size(); i++) {
                PackInputFrame f;
                f.name = m->sheet.frames[i].name;
                f.image = m->images[i];
                f.noTrim = spriteFrameHasBorder(m->sheet.frames[i]);
                pm.frames.push_back(std::move(f));
            }
            packMembers.push_back(std::move(pm));
        }
        PackResult packed = pack(packMembers, settings);
        if (!packed.ok()) {
            result->error = packed.error;
            if (result->error.kind == "too_large") {
                result->error.kind = "frame_too_large";
                // field: the frame (the packer names it "frame '<name>' of '<member>'")
                const size_t open = result->error.message.find("frame '");
                const size_t close = open == std::string::npos ? open : result->error.message.find('\'', open + 7);
                if (close != std::string::npos) result->error.field = result->error.message.substr(open + 7, close - open - 7);
                if (result->error.fix.empty()) result->error.fix = "raise max_size (at most 8192), or split the frame";
            }
            result->ms = elapsed();
            return result;
        }

        // frame names: the member's when unique in the atlas, else "<member path without extension>/<name>"
        // ("<member path>/<name>" when two members differ only in their extension: hero.png, hero.aseprite)
        std::map<std::string, int> nameCount;
        std::map<std::string, int> prefixCount;
        for (const auto& m : members) {
            for (const SpriteFrame& f : m->sheet.frames) nameCount[f.name]++;
            prefixCount[withoutExtension(m->path)]++;
        }
        auto memberPrefix = [&](const std::string& path) { return prefixCount[withoutExtension(path)] > 1 ? path : withoutExtension(path); };
        SpriteSheetData& sheet = result->sheet;
        sheet.source = input.atlasPath;
        sheet.sourceGuid = input.doc.guid;
        bool mixedUnits = false;
        for (size_t i = 0; i < members.size(); i++) {
            if (i == 0) {
                sheet.pixelsPerUnit = members[i]->sheet.pixelsPerUnit;
                sheet.pixelRatio = members[i]->sheet.pixelRatio;
            } else if (members[i]->sheet.pixelsPerUnit != sheet.pixelsPerUnit) {
                mixedUnits = true;
            }
        }
        if (mixedUnits) {
            sheet.pixelsPerUnit = 1.0f;
            result->warnings.push_back({"invalid_settings", "members", "the members have different pixels per unit: the atlas's own sheet uses 1",
                                        "sprites drawing a packed source keep its own; give the members one pixels per unit"});
        }

        // pages: named by their bytes, written only when new
        const std::string exportBase = exportDirOf(*result);
        if (progress) {
            progress->total = static_cast<int>(packed.pages.size());
            progress->done = 0;
        }
        std::vector<int> pageFrames(packed.pages.size(), 0);
        std::vector<long long> pageUsed(packed.pages.size(), 0);
        size_t flat = 0;
        std::map<std::string, long long> memberWaste;
        std::vector<std::pair<std::string, SpriteSheetData>> remaps;
        for (size_t mi = 0; mi < members.size(); mi++) {
            const AtlasMemberFrames& m = *members[mi];
            SpriteSheetData remap = m.sheet;
            for (size_t fi = 0; fi < m.sheet.frames.size(); fi++, flat++) {
                const PackedFrame& pf = packed.layout.frames[flat];
                const SpriteFrame& source = m.sheet.frames[fi];
                SpriteFrame frame = source;
                frame.page = static_cast<uint16_t>(pf.page);
                frame.rect = Rect(static_cast<float>(pf.rect.x), static_cast<float>(pf.rect.y), static_cast<float>(pf.rect.w), static_cast<float>(pf.rect.h));
                frame.offset = Vector2(static_cast<float>(pf.offset.x), static_cast<float>(pf.offset.y));
                frame.sourceSize = Vector2(static_cast<float>(pf.sourceSize.w), static_cast<float>(pf.sourceSize.h));
                remap.frames[fi] = frame;
                SpriteFrame atlasFrame = frame;
                atlasFrame.name = nameCount[source.name] > 1 ? memberPrefix(m.path) + "/" + source.name : source.name;
                atlasFrame.key.clear();
                sheet.frames.push_back(atlasFrame);
                AtlasFrameRow row;
                row.name = atlasFrame.name;
                row.member = m.path;
                row.memberFrame = source.name;
                row.page = pf.page;
                row.rect = pf.rect;
                row.sourceSize = pf.sourceSize;
                row.offset = pf.offset;
                row.waste = pf.sourceSize.w * pf.sourceSize.h - pf.rect.w * pf.rect.h;
                row.duplicateOf = pf.duplicateOf;
                memberWaste[m.path] += row.waste;
                if (pf.duplicateOf >= 0) {
                    result->duplicates++;
                } else if (pf.page >= 0 && static_cast<size_t>(pf.page) < pageFrames.size()) {
                    pageUsed[static_cast<size_t>(pf.page)] += static_cast<long long>(pf.rect.w) * pf.rect.h;
                }
                if (pf.page >= 0 && static_cast<size_t>(pf.page) < pageFrames.size()) pageFrames[static_cast<size_t>(pf.page)]++;
                result->frames.push_back(row);
                if (!result->memberPages.count(m.path)) result->memberPages[m.path] = pf.page;
            }
            remap.pages.clear();
            for (size_t p = 0; p < packed.pages.size(); p++) {
                remap.pages.push_back({std::string(), packed.pages[p].width(), packed.pages[p].height()});
            }
            result->members.push_back(m.path);
            result->memberFiles[m.path] = "member-" + sha256Hex(m.path).substr(0, 16) + ".json";
            // the remap's text: written below, once the pages have their names
            // names, order, sizes, offsets, pivots, tags and slices are the member's: nothing that
            // references the source notices (R2); written once the pages have their names
            remaps.emplace_back(m.path, std::move(remap));
        }
        for (const auto& [member, waste] : memberWaste) result->worstMembers.emplace_back(member, static_cast<int>(std::min<long long>(waste, INT32_MAX)));
        std::stable_sort(result->worstMembers.begin(), result->worstMembers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

        for (size_t p = 0; p < packed.pages.size(); p++) {
            if (cancelled()) {
                result->error = {"cancelled", "", "the build was cancelled", "build it again"};
                return result;
            }
            const PixelImage& image = packed.pages[p];
            AtlasPageRow row;
            row.width = image.width();
            row.height = image.height();
            row.frames = pageFrames[p];
            const double area = static_cast<double>(row.width) * static_cast<double>(row.height);
            row.used = area > 0 ? static_cast<double>(pageUsed[p]) / area : 0.0;
            const auto pixels = image.pixels();
            row.hash = sha256Hex(std::to_string(row.width) + "x" + std::to_string(row.height) + "|" +
                                 sha256Hex(std::string_view(reinterpret_cast<const char*>(pixels.data()), pixels.size()))).substr(0, 16);
            row.file = "page" + std::to_string(p) + "-" + row.hash + ".png";
            if (!input.outDir.empty()) {
                const fs::path file = input.outDir / row.file;
                std::error_code ec;
                if (!fs::exists(file, ec)) {
                    fs::create_directories(input.outDir, ec);
                    if (!image.savePng(file)) {
                        result->error = {"write_failed", "", "cannot write " + file.generic_string(), "check the project's .jasperengine folder is writable"};
                        return result;
                    }
                    row.written = true;
                }
            }
            sheet.pages.push_back({exportBase + row.file, row.width, row.height});
            result->pages.push_back(row);
            if (progress) progress->done = static_cast<int>(p + 1);
        }
        result->usage = packed.usage;
        // R29: the web family's page sizes, when its own max size lays the frames out otherwise
        if (input.platform == "desktop" && input.doc.settings.maxSizeFor("web") != settings.maxSize && !cancelled()) {
            PackSettings web = settings;
            web.maxSize = input.doc.settings.maxSizeFor("web");
            const PackResult webPacked = pack(packMembers, web);
            if (webPacked.ok()) {
                for (const PixelImage& page : webPacked.pages) result->webPages.emplace_back(page.width(), page.height());
            }
        }

        // the layout against the previous build of this atlas
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = results.find(input.atlasPath);
            if (it != results.end() && it->second->platform == input.platform && it->second->frames.size() == result->frames.size() &&
                it->second->pages.size() == result->pages.size()) {
                bool same = true;
                for (size_t i = 0; same && i < result->frames.size(); i++) {
                    same = it->second->frames[i].rect == result->frames[i].rect && it->second->frames[i].page == result->frames[i].page &&
                           it->second->frames[i].member == result->frames[i].member;
                }
                result->relayout = !same;
            }
        }

        if (!input.outDir.empty()) {
            for (auto& [member, remap] : remaps) {
                for (size_t p = 0; p < remap.pages.size(); p++) remap.pages[p].texture = sheet.pages[p].texture;
                if (!writeCacheText(input.outDir / result->memberFiles[member], writeSheet(remap))) {
                    result->error = {"write_failed", member, "cannot write the remap of " + member, "check the project's .jasperengine folder is writable"};
                    return result;
                }
            }
            if (!writeCacheText(result->sheetFile(), writeSheet(sheet))) {
                result->error = {"write_failed", "", "cannot write the atlas's sheet", "check the project's .jasperengine folder is writable"};
                return result;
            }
        } else {
            result->pageImages = std::move(packed.pages);
        }
        result->ok = true;
        result->ms = elapsed();
        if (!input.outDir.empty() && !writeCacheText(input.outDir / "build.json", buildJson(*result).dump(1) + "\n")) {
            result->ok = false;
            result->error = {"write_failed", "", "cannot write build.json", "check the project's .jasperengine folder is writable"};
        }
        return result;
    }


    void SpriteAtlasBuilder::start(const std::string& atlasPath) {
        if (!project || currentMode == AtlasMode::Disabled) return;
        PixelProblem problem;
        std::optional<SpriteAtlasDocument> doc = readDocument(project->getAssetsPath() / atlasPath, &problem);
        if (!doc) {
            auto failed = std::make_shared<AtlasBuildResult>();
            failed->atlasPath = atlasPath;
            failed->error = problem;
            std::lock_guard<std::mutex> lock(mutex);
            attempts[atlasPath] = failed;
            builds[atlasPath]++;
            return;
        }
        AtlasBuildInput input;
        gather(*project, atlasPath, *doc, "desktop", input);
        for (const PixelProblem& p : input.problems) {
            if (p.kind == "importing") {
                // built once its .aseprite members are imported (the importer's listener)
                waitingImports.insert(atlasPath);
                return;
            }
        }
        waitingImports.erase(atlasPath);
        {
            std::lock_guard<std::mutex> lock(mutex);
            included[atlasPath] = doc->includeInBuild;
        }
        Running r;
        r.progress = std::make_shared<AtlasProgress>();
        r.cancel = std::make_shared<jobs::CancelToken>();
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "sprite-atlas-build";
        options.tags = jobs::Tag::Asset;
        options.cancel = r.cancel.get();
        auto progress = r.progress;
        auto cancel = r.cancel;
        r.task = jobs::submit([this, input, progress, cancel]() -> std::shared_ptr<AtlasBuildResult> {
            struct Wake {
                ~Wake() { getEditorHost().enqueueMainThreadTask([]() {}); }
            } wake;
            try {
                return build(input, progress.get(), cancel.get());
            } catch (const std::exception& e) {
                auto failed = std::make_shared<AtlasBuildResult>();
                failed->atlasPath = input.atlasPath;
                failed->error = {"build_failed", "", e.what(), "build it again"};
                return failed;
            }
        }, options);
        running[atlasPath] = std::move(r);
    }

    void SpriteAtlasBuilder::adopt(const std::shared_ptr<AtlasBuildResult>& result) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            attempts[result->atlasPath] = result;
            builds[result->atlasPath]++;
        }
        if (!result->ok) {
            if (result->error.kind != "cancelled") {
                Out::error("[Sprite Atlas] %s: %s%s", result->atlasPath.c_str(), result->error.message.c_str(),
                           result->error.fix.empty() ? "" : (" (" + result->error.fix + ")").c_str());
            }
            return;
        }
        // the pages load before anything shows them (the previous build stays drawn until then)
        swaps.erase(std::remove_if(swaps.begin(), swaps.end(), [&](const Swap& s) { return s.result->atlasPath == result->atlasPath; }), swaps.end());
        Swap swap;
        swap.result = result;
        for (size_t p = 0; p < result->pages.size(); p++) {
            auto texture = std::make_shared<Texture>(result->pageFile(p).string());
            texture->load();
            swap.pages.push_back(texture);
        }
        swaps.push_back(std::move(swap));
    }

    void SpriteAtlasBuilder::finishSwap(Swap& swap) {
        const AtlasBuildResult& r = *swap.result;
        std::set<std::string> affected(r.members.begin(), r.members.end());
        std::set<std::string> keep;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto previous = results.find(r.atlasPath);
            if (previous != results.end()) affected.insert(previous->second->members.begin(), previous->second->members.end());
            results[r.atlasPath] = swap.result;
        }
        // files of earlier builds: gone once nothing names them, and never while another build of
        // this atlas is running or queued (it may have written pages this one does not name)
        bool pending = running.count(r.atlasPath) || again.count(r.atlasPath) || waitingImports.count(r.atlasPath);
        for (const Swap& other : swaps) pending = pending || (&other != &swap && other.result->atlasPath == r.atlasPath);
        if (!pending) removeStaleFiles(r);
        refreshRemaps();
        SpriteSheetPool::invalidate(r.atlasPath);
        for (const std::string& member : affected) SpriteSheetPool::invalidate(member);
        if (project) {
            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) sceneProject->needUpdateRender = true;
        }
        if (!r.cacheHit) {
            Out::info("[Sprite Atlas] Built %s: %zu %s, %s frames, %d %% (%.0f ms)", r.atlasPath.c_str(), r.pages.size(), r.pages.size() == 1 ? "page" : "pages",
                      thousands(static_cast<long long>(r.frames.size())).c_str(), static_cast<int>(std::lround(r.usage * 100.0)), r.ms);
        }
        for (const PixelProblem& w : r.warnings) Out::warning("[Sprite Atlas] %s: %s", r.atlasPath.c_str(), w.message.c_str());
    }

    fs::path SpriteAtlasBuilder::exportDir(const AtlasBuildInput& input) {
        return input.outDir.empty() ? fs::path() : input.outDir.parent_path() / ("export-" + input.platform);
    }

    void SpriteAtlasBuilder::removeStaleFiles(const AtlasBuildResult& r) {
        if (r.dir.empty()) return;
        std::set<std::string> keep;
        auto keepBuild = [&](const AtlasBuildResult& b) {
            for (const AtlasPageRow& p : b.pages) keep.insert(p.file);
            for (const auto& [member, file] : b.memberFiles) keep.insert(file);
        };
        keepBuild(r);
        // the newest build written to the folder (a later build than this one, adopted after it)
        AtlasBuildResult newest;
        const nlohmann::json j = nlohmann::json::parse(readText(r.dir / "build.json"), nullptr, false);
        if (!j.is_discarded() && fromBuildJson(j, newest)) keepBuild(newest);
        std::error_code ec;
        for (const fs::directory_entry& entry : fs::directory_iterator(r.dir, ec)) {
            const std::string name = entry.path().filename().string();
            if (!keep.count(name) && (name.rfind("page", 0) == 0 || name.rfind("member-", 0) == 0)) fs::remove(entry.path(), ec);
        }
    }

    void SpriteAtlasBuilder::refreshRemaps() {
        std::map<std::string, std::string> newRemaps;
        std::map<std::string, std::vector<std::string>> newPackers;
        std::set<std::string> changed;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (currentMode == AtlasMode::Enabled) {
                // results is ordered by path: the first atlas packing a member wins (R18)
                for (const auto& [atlas, r] : results) {
                    auto inc = included.find(atlas);
                    if (inc != included.end() && !inc->second) continue;
                    for (const std::string& member : r->members) {
                        newPackers[member].push_back(atlas);
                        newRemaps.emplace(member, atlas);
                    }
                }
            }
            for (const auto& [member, atlas] : remaps) {
                auto it = newRemaps.find(member);
                if (it == newRemaps.end() || it->second != atlas) changed.insert(member);
            }
            for (const auto& [member, atlas] : newRemaps) {
                auto it = remaps.find(member);
                if (it == remaps.end() || it->second != atlas) changed.insert(member);
            }
            remaps = std::move(newRemaps);
            packers = std::move(newPackers);
        }
        for (const std::string& member : changed) SpriteSheetPool::invalidate(member);
        if (!changed.empty() && project) {
            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) sceneProject->needUpdateRender = true;
        }
    }

    bool SpriteAtlasBuilder::update() {
        bool changed = false;
        for (auto it = running.begin(); it != running.end();) {
            if (!it->second.task.isReady()) {
                ++it;
                continue;
            }
            std::shared_ptr<AtlasBuildResult> result;
            if (it->second.task.status() == jobs::TaskStatus::Completed) result = it->second.task.get();
            const std::string path = it->first;
            it = running.erase(it);
            if (!result) {
                result = std::make_shared<AtlasBuildResult>();
                result->atlasPath = path;
                result->error = {"cancelled", "", "the build was cancelled", "build it again"};
            }
            adopt(result);
            changed = true;
            if (again.erase(path)) start(path);
        }
        for (auto it = swaps.begin(); it != swaps.end();) {
            bool ready = true;
            for (const std::shared_ptr<Texture>& texture : it->pages) {
                const ResourceLoadState state = texture->load().state;
                if (state == ResourceLoadState::Loading || state == ResourceLoadState::NotStarted) ready = false;
            }
            if (!ready) {
                ++it;
                continue;
            }
            finishSwap(*it);
            it = swaps.erase(it);
            changed = true;
        }
        // atlases waiting for their .aseprite members' imports
        if (!waitingImports.empty()) {
            pixelart::aseprite::AsepriteImporter& importer = pixelart::aseprite::AsepriteImporter::get();
            const std::set<std::string> waiting = waitingImports;
            for (const std::string& atlas : waiting) {
                std::optional<SpriteAtlasDocument> doc = project ? readDocument(project->getAssetsPath() / atlas, nullptr) : std::nullopt;
                if (!doc) {
                    waitingImports.erase(atlas);
                    continue;
                }
                AtlasBuildInput input;
                gather(*project, atlas, *doc, "desktop", input);
                bool pending = false;
                for (const PixelProblem& p : input.problems) pending = pending || (p.kind == "importing" && importer.isPending(p.field));
                if (!pending) {
                    waitingImports.erase(atlas);
                    rebuild(atlas);
                    changed = true;
                }
            }
        }
        return changed;
    }

    bool SpriteAtlasBuilder::isBuilding(const std::string& path) const {
        const std::string atlasPath = assetPathOf(path);
        if (running.count(atlasPath) || waitingImports.count(atlasPath)) return true;
        for (const Swap& s : swaps) {
            if (s.result->atlasPath == atlasPath) return true;
        }
        return false;
    }

    bool SpriteAtlasBuilder::progressOf(const std::string& path, int& done, int& total) const {
        auto it = running.find(assetPathOf(path));
        if (it == running.end()) return false;
        done = it->second.progress->done.load();
        total = it->second.progress->total.load();
        return true;
    }

    std::string SpriteAtlasBuilder::statusText() const {
        for (const auto& [path, r] : running) {
            const std::string name = fs::path(path).stem().string();
            const int total = r.progress->total.load();
            if (total <= 0) return "Building sprite atlas " + name;
            return "Building sprite atlas " + name + " (" + std::to_string(r.progress->done.load()) + "/" + std::to_string(total) + " pages)";
        }
        return std::string();
    }

    void SpriteAtlasBuilder::cancelAll() {
        again.clear();
        waitingImports.clear();
        for (auto& [path, r] : running) r.cancel->cancel();
    }

    bool SpriteAtlasBuilder::waitFor(const std::vector<std::string>& paths, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            pixelart::aseprite::AsepriteImporter::get().update();
            update();
            bool busy = false;
            for (const std::string& p : paths) busy = busy || isBuilding(p) || again.count(assetPathOf(p));
            if (!busy) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            jobs::pump(1.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    uint64_t SpriteAtlasBuilder::buildCount(const std::string& path) const {
        const std::string atlasPath = assetPathOf(path);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = builds.find(atlasPath);
        return it == builds.end() ? 0 : it->second;
    }

    std::shared_ptr<const AtlasBuildResult> SpriteAtlasBuilder::result(const std::string& path) const {
        const std::string atlasPath = assetPathOf(path);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = results.find(atlasPath);
        return it == results.end() ? nullptr : it->second;
    }

    std::shared_ptr<const AtlasBuildResult> SpriteAtlasBuilder::lastAttempt(const std::string& path) const {
        const std::string atlasPath = assetPathOf(path);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = attempts.find(atlasPath);
        return it == attempts.end() ? nullptr : it->second;
    }

    std::vector<std::string> SpriteAtlasBuilder::atlasesPacking(const std::string& memberPath) const {
        const std::string member = assetPathOf(memberPath);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = packers.find(member);
        return it == packers.end() ? std::vector<std::string>() : it->second;
    }

    std::string SpriteAtlasBuilder::packedIn(const std::string& memberPath, int* page) const {
        const std::string member = assetPathOf(memberPath);
        std::lock_guard<std::mutex> lock(mutex);
        std::string atlas;
        auto remap = remaps.find(member);
        if (remap != remaps.end()) {
            atlas = remap->second;
        } else {
            // packed by an atlas not in use (Include in Build off, a mode other than Enabled)
            for (const auto& [path, r] : results) {
                if (std::find(r->members.begin(), r->members.end(), member) != r->members.end()) {
                    atlas = path;
                    break;
                }
            }
        }
        if (!atlas.empty() && page) {
            auto r = results.find(atlas);
            auto p = r != results.end() ? r->second->memberPages.find(member) : std::map<std::string, int>::const_iterator();
            *page = r != results.end() && p != r->second->memberPages.end() ? p->second : 0;
        }
        return atlas;
    }

    std::vector<PixelProblem> SpriteAtlasBuilder::crossWarnings(const std::string& path) const {
        const std::string atlasPath = assetPathOf(path);
        std::vector<PixelProblem> out;
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [member, atlases] : packers) {
            if (atlases.size() < 2 || std::find(atlases.begin(), atlases.end(), atlasPath) == atlases.end()) continue;
            std::string others;
            for (const std::string& a : atlases) {
                if (a != atlasPath) others += (others.empty() ? "" : ", ") + a;
            }
            out.push_back({"double_packed", member, member + " is packed by " + others + " too; " + atlases[0] + " draws it (its path sorts first)",
                           "remove " + member + " from all but one atlas (edit_sprite_atlas remove)"});
        }
        return out;
    }

    bool SpriteAtlasBuilder::resolve(const std::string& sourceRef, std::string& fileOut) const {
        const size_t hash = sourceRef.find('#');
        if (hash == std::string::npos) return false;
        const std::string source = sourceRef.substr(0, hash);
        const std::string key = sourceRef.substr(hash + 1);
        std::shared_ptr<const AtlasBuildResult> r;
        bool member = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto atlas = results.find(source);
            if (atlas != results.end()) {
                r = atlas->second;
            } else {
                auto remap = remaps.find(source);
                if (remap != remaps.end()) {
                    auto packer = results.find(remap->second);
                    if (packer != results.end()) {
                        r = packer->second;
                        member = true;
                    }
                }
            }
        }
        if (r && r->ok) {
            if (key == "sheet") {
                fileOut = (member ? r->memberFile(source) : r->sheetFile()).string();
                return true;
            }
            if (key == "atlas" && member) {
                fileOut = r->sheetFile().string();
                return true;
            }
            if (key.rfind("image#", 0) == 0) {
                size_t page = 0;
                const std::string index = key.substr(6);
                const auto parsed = std::from_chars(index.data(), index.data() + index.size(), page);
                if (index.empty() || parsed.ec != std::errc() || parsed.ptr != index.data() + index.size() || page >= r->pages.size()) return false;
                fileOut = r->pageFile(page).string();
                return true;
            }
            if (!member) return false;
        }
        // a texture with a Sprite Mode (its own frames)
        if (key == "sheet") return SpriteSheetBuilder::get().resolve(source, fileOut);
        return false;
    }

}
