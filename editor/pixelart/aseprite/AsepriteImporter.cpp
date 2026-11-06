// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AsepriteImporter.h"

#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "command/type/SetAssetMetaCmd.h"
#include "sprite/SpritePacker.h"

#include "asset/ImportedAssets.h"
#include "reload/HotReloadService.h"
#include "io/AtomicFile.h"
#include "jobs/JobSystem.h"
#include "pool/AnimationClipPool.h"
#include "sprite/SpriteSheetIO.h"
#include "sprite/SpriteSheetPool.h"
#include "texture/Texture.h"
#include "util/Sha256.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

namespace fs = std::filesystem;

namespace jasperengine::editor::pixelart::aseprite {

    namespace {

        constexpr const char* kRecordFormat = "jasperengine.aseprite-import";
        constexpr int kRecordVersion = 1;

        bool writeText(const fs::path& path, const std::string& text) {
            io::AtomicWriteOptions options;
            options.durability = io::Durability::Cache;
            return io::writeFileAtomic(path, text, options).ok();
        }

        nlohmann::json problemJson(const PixelProblem& p) {
            nlohmann::json j = {{"kind", p.kind}, {"message", p.message}};
            if (!p.fix.empty()) j["fix"] = p.fix;
            if (!p.field.empty()) j["field"] = p.field;
            return j;
        }

        PixelProblem problemFrom(const nlohmann::json& j) {
            return {j.value("kind", std::string()), j.value("field", std::string()), j.value("message", std::string()), j.value("fix", std::string())};
        }

        const char* kindName(SubAssetKind kind) { return subAssetKindName(kind); }

        // import.json: everything a cache hit needs, without the pixels
        nlohmann::json recordJson(const ImportRecord& r) {
            nlohmann::json clips = nlohmann::json::array();
            for (const ImportRecord::Clip& c : r.clips) clips.push_back({{"key", c.key}, {"name", c.name}, {"file", c.file}, {"length", c.length}, {"wrap", c.wrap}});
            nlohmann::json outputs = nlohmann::json::array();
            for (const ImportRecord::Output& o : r.outputs) outputs.push_back({{"key", o.key}, {"name", o.name}, {"kind", o.kind}});
            nlohmann::json warnings = nlohmann::json::array();
            for (const PixelProblem& w : r.warnings) warnings.push_back(problemJson(w));
            nlohmann::json pages = nlohmann::json::array();
            for (const auto& [w, h] : r.pages) pages.push_back({w, h});
            return {{"format", kRecordFormat}, {"version", kRecordVersion}, {"key", r.key}, {"source", r.assetPath}, {"frames", r.frames},
                    {"pages", pages}, {"clips", clips}, {"sub_assets", outputs}, {"tags", r.tags}, {"slices", r.slices},
                    {"warnings", warnings}, {"summary", r.summary}, {"settings", r.settings}, {"ms", r.ms}};
        }

        bool recordFromJson(const nlohmann::json& j, ImportRecord& r) {
            if (!j.is_object() || j.value("format", std::string()) != kRecordFormat || j.value("version", 0) != kRecordVersion) return false;
            r.frames = j.value("frames", 0);
            for (const auto& p : j.value("pages", nlohmann::json::array())) {
                if (p.is_array() && p.size() == 2) r.pages.emplace_back(p[0].get<int>(), p[1].get<int>());
            }
            for (const auto& c : j.value("clips", nlohmann::json::array())) {
                r.clips.push_back({c.value("key", std::string()), c.value("name", std::string()), c.value("file", std::string()),
                                   c.value("length", 0.0f), c.value("wrap", std::string())});
            }
            for (const auto& o : j.value("sub_assets", nlohmann::json::array())) {
                r.outputs.push_back({o.value("key", std::string()), o.value("name", std::string()), o.value("kind", std::string()), Guid()});
            }
            for (const auto& w : j.value("warnings", nlohmann::json::array())) r.warnings.push_back(problemFrom(w));
            r.tags = j.value("tags", std::vector<std::string>());
            r.slices = j.value("slices", std::vector<std::string>());
            r.summary = j.value("summary", std::string());
            r.settings = j.value("settings", nlohmann::json::object());
            r.ok = true;
            return true;
        }

        void assignGuids(ImportRecord& r, const ImportRequest& request) {
            for (ImportRecord::Output& o : r.outputs) {
                auto named = request.namedGuids.find(o.key);
                o.guid = named != request.namedGuids.end() ? named->second : Guid::derive(request.sourceGuid, o.key);
            }
        }

        // the pixels per unit of the project (S41 adds the setting; until then 1 pixel = 1 unit)
        int projectPixelsPerUnit(const Project&) { return 1; }

    }

    nlohmann::json ImportRecord::toJson() const {
        nlohmann::json clipsJson = nlohmann::json::array();
        for (const Clip& c : clips) {
            Guid guid;
            for (const Output& o : outputs) {
                if (o.key == c.key) guid = o.guid;
            }
            clipsJson.push_back({{"name", c.name}, {"key", c.key}, {"reference", assetPath + "#" + c.name}, {"guid", guid.str()},
                                 {"length", c.length}, {"wrap", c.wrap}});
        }
        nlohmann::json outputsJson = nlohmann::json::array();
        for (const Output& o : outputs) outputsJson.push_back({{"key", o.key}, {"name", o.name}, {"kind", o.kind}, {"guid", o.guid.str()}});
        nlohmann::json warningsJson = nlohmann::json::array();
        for (const PixelProblem& w : warnings) warningsJson.push_back(problemJson(w));
        nlohmann::json pagesJson = nlohmann::json::array();
        for (const auto& [w, h] : pages) pagesJson.push_back({w, h});
        nlohmann::json j = {{"sheet", assetPath}, {"frames", frames}, {"pages", pagesJson}, {"clips", clipsJson}, {"tags", tags},
                            {"slices", slices}, {"sub_assets", outputsJson}, {"warnings", warningsJson}, {"summary", summary},
                            {"cache", {{"key", key}, {"hit", cacheHit}}}};
        if (!ok) j["error"] = problemJson(error);
        return j;
    }

    const ImportRecord::Clip* ImportRecord::findClip(const std::string& keyOrName) const {
        for (const Clip& c : clips) {
            if (c.key == keyOrName || c.key == "animation:" + keyOrName) return &c;
        }
        for (const Clip& c : clips) {
            if (c.name == keyOrName) return &c;
        }
        return nullptr;
    }

    AsepriteImporter& AsepriteImporter::get() {
        static AsepriteImporter* importer = new AsepriteImporter();
        return *importer;
    }

    fs::path AsepriteImporter::cacheRoot() const {
        return project ? project->getProjectInternalPath() / "cache" / "imported" / "v1" : fs::path();
    }

    std::string AsepriteImporter::assetPathOf(const std::string& path) const {
        if (!project) return std::string();
        fs::path p(path);
        if (!p.is_absolute()) p = project->getAssetsPath() / p;
        const fs::path relative = p.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal());
        const std::string text = relative.generic_string();
        if (text.empty() || text == "." || text.rfind("..", 0) == 0) return std::string();
        return text;
    }

    void AsepriteImporter::attach(Project* newProject) {
        if (newProject == project) return;
        if (project) {
            for (auto& [path, task] : running) task.wait();
            running.clear();
            again.clear();
            swaps.clear();
            waiting.clear();   // their scenes go with the project
            ImportedAssets::setProvider(nullptr);
            SetAssetMetaCmd::removeWriteListener(this);
            std::lock_guard<std::mutex> lock(mutex);
            records.clear();
            attempts.clear();
            attemptCounts.clear();
            adoptedSequences.clear();
            started = 0;
        }
        project = newProject;
        if (!project) return;
        ImportedAssets::setProvider([this](const std::string& ref, std::string& out) { return resolve(ref, out); });
        // an import settings edit (and its undo) imports the file again
        SetAssetMetaCmd::addWriteListener(this, [this](Project* writer, const fs::path& asset, const std::string& key) {
            if (writer != project || key.rfind("import", 0) != 0) return;
            if (!SpriteSheetPool::isAsepritePath(asset.generic_string())) return;
            reimport((project->getProjectPath() / asset).string());
        });
        requestAll();
    }

    void AsepriteImporter::requestAll() {
        if (!project) return;
        std::vector<std::string> paths;
        project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
            if (info.type == AssetType::AsepriteSprite) paths.push_back((project->getProjectPath() / info.path).string());
        });
        for (const std::string& p : paths) reimport(p);
    }

    AsepriteImportSettings AsepriteImporter::settingsOf(const std::string& assetPath, std::vector<PixelProblem>* problems) const {
        AsepriteImportSettings settings;
        if (!project) return settings;
        const YAML::Node node = SetAssetMetaCmd::read(project->getAssetsPath() / assetPath, kSettingsKey);
        std::vector<PixelProblem> local;
        AsepriteImportSettings::fromYaml(node, settings, local);
        if (problems) *problems = std::move(local);
        return settings;
    }

    std::optional<ImportRequest> AsepriteImporter::requestOf(const std::string& path) const {
        const std::string assetPath = assetPathOf(path);
        if (!project || assetPath.empty()) return std::nullopt;
        ImportRequest request;
        static std::atomic<uint64_t> requests{0};
        request.sequence = ++requests;
        request.file = (project->getAssetsPath() / assetPath).lexically_normal();
        request.assetPath = assetPath;
        request.settings = settingsOf(assetPath);
        request.projectPixelsPerUnit = projectPixelsPerUnit(*project);
        request.cacheRoot = cacheRoot();
        AssetDatabase::AssetInfo info;
        if (project->getAssetDatabase().getInfo(project->normalizeToProjectRelative(request.file), info)) {
            request.sourceGuid = info.guid;
            for (const AssetDatabase::SubAsset& sub : info.subAssets) request.namedGuids[sub.key] = sub.guid;
        }
        return request;
    }

    std::string AsepriteImporter::cacheKey(const std::string& contentSha256, const ImportRequest& request) {
        // the outputs carry the file's path and GUID (the sheet's source, frame and untagged clip
        // names, the clips' source.path): a copy or a renamed file imports on its own
        const std::string text = contentSha256 + "|path " + request.assetPath + "|guid " + request.sourceGuid.str() + "|" +
                                 request.settings.fingerprint() + "|ppu " +
                                 std::to_string(request.settings.pixelsPerUnit.value_or(request.projectPixelsPerUnit)) + "|importer " +
                                 std::to_string(kAsepriteImporterVersion) + "|packer " + std::to_string(sprite::kPackerVersion) +
                                 "|sheet " + std::to_string(SPRITESHEET_VERSION);
        return sha256Hex(text);
    }

    std::shared_ptr<ImportRecord> AsepriteImporter::runImport(const ImportRequest& request) {
        const auto t0 = std::chrono::steady_clock::now();
        auto record = std::make_shared<ImportRecord>();
        record->assetPath = request.assetPath;
        record->sourceGuid = request.sourceGuid;
        record->sequence = request.sequence;
        record->settings = request.settings.toJson();

        std::ifstream in(request.file, std::ios::binary);
        if (!in) throw std::runtime_error("cannot read the file");
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        record->key = cacheKey(sha256Hex(bytes), request);
        record->dir = request.cacheRoot / record->key.substr(0, 2) / record->key;

        // a cache hit: import.json is written last, so its presence means the whole import is there
        {
            std::ifstream recordIn(record->dir / "import.json", std::ios::binary);
            if (recordIn) {
                const nlohmann::json j = nlohmann::json::parse(recordIn, nullptr, false);
                ImportRecord cached = *record;
                if (!j.is_discarded() && recordFromJson(j, cached) && fs::exists(cached.sheetFile())) {
                    *record = std::move(cached);
                    record->cacheHit = true;
                    assignGuids(*record, request);
                    record->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    return record;
                }
            }
        }

        ReadOptions options;
        options.provenance = false;
        ReadResult read = aseprite::read(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), options);
        if (!read.ok()) {
            // half written or broken: the hot reload service keeps the previous version and tries again
            throw std::runtime_error(read.error.message.empty() ? read.error.kind : read.error.message);
        }
        for (const CodecError& w : read.warnings) record->warnings.push_back(w);

        AsepriteImportContext ctx;
        ctx.sourcePath = request.assetPath;
        ctx.sourceGuid = request.sourceGuid;
        ctx.projectPixelsPerUnit = request.projectPixelsPerUnit;
        const AsepriteImport import = AsepriteImport::run(*read.document, request.settings, ctx);
        record->warnings.insert(record->warnings.end(), import.warnings.begin(), import.warnings.end());
        if (!import.ok()) {
            record->ok = false;
            record->error = import.error;
            return record;
        }

        // the pages and clips first, the sheet, then import.json: a reader never sees half an import
        const std::string exportDir = std::string(ImportedAssets::kImportedDir) + "/" + record->key.substr(0, 2) + "/" + record->key + "/";
        SpriteSheetData sheet = import.sheet;
        for (size_t p = 0; p < import.pages.size(); p++) {
            sheet.pages[p].texture = exportDir + "page" + std::to_string(p) + ".png";
            if (!import.pages[p].savePng(record->pageFile(p))) throw std::runtime_error("cannot write " + record->pageFile(p).string());
            record->pages.emplace_back(import.pages[p].width(), import.pages[p].height());
        }
        for (size_t c = 0; c < import.clips.size(); c++) {
            const ClipOutput& clip = import.clips[c];
            const std::string file = "clip-" + std::to_string(c) + ".anim";
            if (!writeText(record->dir / file, clip.text)) throw std::runtime_error("cannot write " + (record->dir / file).string());
            record->clips.push_back({clip.key, clip.name, file, clip.length, clip.wrap});
        }
        if (!writeText(record->sheetFile(), writeSheet(sheet))) throw std::runtime_error("cannot write " + record->sheetFile().string());
        record->frames = static_cast<int>(sheet.frames.size());
        for (const SubAssetOutput& o : import.subAssets) record->outputs.push_back({o.key, o.name, kindName(o.kind), Guid()});
        for (const SpriteTag& t : sheet.tags) record->tags.push_back(t.name);
        for (const SpriteSlice& s : sheet.slices) record->slices.push_back(s.name);
        record->summary = import.summary();
        record->ok = true;
        record->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!writeText(record->dir / "import.json", recordJson(*record).dump(2) + "\n")) {
            throw std::runtime_error("cannot write " + (record->dir / "import.json").string());
        }
        assignGuids(*record, request);
        return record;
    }

    void AsepriteImporter::reimport(const std::string& path) {
        const std::string assetPath = assetPathOf(path);
        if (assetPath.empty() || !SpriteSheetPool::isAsepritePath(assetPath)) return;
        if (running.count(assetPath)) {
            again.insert(assetPath);
            return;
        }
        start(assetPath);
    }

    void AsepriteImporter::start(const std::string& assetPath) {
        std::optional<ImportRequest> request = requestOf(assetPath);
        if (!request) return;
        started++;
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "aseprite-import";
        options.tags = jobs::Tag::Asset;
        ImportRequest req = *request;
        running[assetPath] = jobs::submit([req]() -> std::shared_ptr<ImportRecord> {
            // the idle editor loop wakes to adopt it
            struct Wake {
                ~Wake() { getEditorHost().enqueueMainThreadTask([]() {}); }
            } wake;
            try {
                return runImport(req);
            } catch (const std::exception& e) {
                auto failed = std::make_shared<ImportRecord>();
                failed->assetPath = req.assetPath;
                failed->sourceGuid = req.sourceGuid;
                failed->sequence = req.sequence;
                failed->error = {"import_failed", "", e.what(), "save the file again from Aseprite"};
                return failed;
            }
        }, options);
    }

    std::shared_ptr<const ImportRecord> AsepriteImporter::lastAttempt(const std::string& path) const {
        const std::string assetPath = assetPathOf(path);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = attempts.find(assetPath);
        return it == attempts.end() ? nullptr : it->second;
    }

    uint64_t AsepriteImporter::attemptCount(const std::string& path) const {
        const std::string assetPath = assetPathOf(path);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = attemptCounts.find(assetPath);
        return it == attemptCounts.end() ? 0 : it->second;
    }

    void AsepriteImporter::adopt(std::shared_ptr<ImportRecord> record) {
        if (!record || !project) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            attempts[record->assetPath] = record;
            attemptCounts[record->assetPath]++;
            // the watcher's reload and the importer's own job can import a file at once and finish
            // in either order: an import requested before the one adopted is out of date
            uint64_t& newest = adoptedSequences[record->assetPath];
            if (record->sequence < newest) return;
            newest = record->sequence;
        }
        // R36: what an import found reaches the next MCP call's external_changes.problems, as a
        // clip's load warnings do (the path project-relative, as the auto-sync names files)
        const std::string projectPath = project->normalizeToProjectRelative(project->getAssetsPath() / record->assetPath).generic_string();
        auto late = [&](const PixelProblem& p, const char* severity) {
            HotReloadService::get().addLateProblem({{"path", projectPath}, {"severity", severity},
                                                    {"message", p.kind + ": " + p.message + (p.fix.empty() ? "" : " (" + p.fix + ")")}});
        };
        for (const PixelProblem& w : record->warnings) late(w, "warning");
        if (!record->ok) {
            late(record->error, "error");
            // the previous version stays (R22)
            Out::warning("[Aseprite] %s: %s (kept the previous version)", record->assetPath.c_str(), record->error.message.c_str());
            std::lock_guard<std::mutex> lock(mutex);
            if (!records.count(record->assetPath)) records[record->assetPath] = record;
            return;
        }
        // nor does an earlier import still loading its pages take over once they load
        swaps.erase(std::remove_if(swaps.begin(), swaps.end(), [&](const Swap& s) { return s.record->assetPath == record->assetPath; }),
                    swaps.end());
        Swap swap;
        swap.record = std::move(record);
        // the pages load before anything shows them: the old version stays drawn until then
        for (size_t p = 0; p < swap.record->pages.size(); p++) {
            auto texture = std::make_shared<Texture>(swap.record->pageFile(p).string());
            texture->load();
            swap.pages.push_back(texture);
        }
        swaps.push_back(std::move(swap));
    }

    void AsepriteImporter::finishSwap(Swap& swap) {
        const ImportRecord& r = *swap.record;
        bool first;
        {
            std::lock_guard<std::mutex> lock(mutex);
            first = !records.count(r.assetPath) || !records[r.assetPath]->ok;
            records[r.assetPath] = swap.record;
        }
        SpriteSheetPool::invalidate(r.assetPath);
        AnimationClipPool::invalidateModel(r.assetPath);
        // an idle Scene view updates its systems only when flagged: the sprites showing the file
        // pick the new sheet up on the next frame (R22)
        if (project) {
            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) sceneProject->needUpdateRender = true;
        }
        if (!r.cacheHit || !first) {
            Out::info("[Aseprite] Imported %s: %d frames, %zu clips (%.0f ms)", r.assetPath.c_str(), r.frames, r.clips.size(), r.ms);
        }
        for (const PixelProblem& w : r.warnings) Out::warning("[Aseprite] %s: %s", r.assetPath.c_str(), w.message.c_str());
    }

    bool AsepriteImporter::update() {
        bool changed = false;
        for (auto it = running.begin(); it != running.end();) {
            if (!it->second.isReady()) {
                ++it;
                continue;
            }
            std::shared_ptr<ImportRecord> record;
            if (it->second.status() == jobs::TaskStatus::Completed) record = it->second.get();
            const std::string path = it->first;
            it = running.erase(it);
            adopt(record);
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
        // the callers waiting for a file whose import is over (it may have failed)
        for (size_t i = 0; i < waiting.size();) {
            if (isPending(waiting[i].first)) {
                i++;
                continue;
            }
            std::function<void()> then = std::move(waiting[i].second);
            waiting.erase(waiting.begin() + static_cast<std::ptrdiff_t>(i));
            then();
        }
        return changed;
    }

    bool AsepriteImporter::waitFor(const std::vector<std::string>& paths, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            update();
            bool busy = false;
            for (const std::string& p : paths) busy = busy || isPending(p);
            if (!busy) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            jobs::pump(1.0);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    bool AsepriteImporter::ensureImport(const std::string& path) {
        const std::string assetPath = assetPathOf(path);
        if (isPending(assetPath)) return false;
        if (record(assetPath)) return true;
        reimport(assetPath);
        return !isPending(assetPath) && record(assetPath) != nullptr;
    }

    void AsepriteImporter::whenImported(const std::string& path, std::function<void()> then) {
        const std::string assetPath = assetPathOf(path);
        if (ensureImport(assetPath) || !isPending(assetPath)) {
            then();
            return;
        }
        waiting.emplace_back(assetPath, std::move(then));
    }

    bool AsepriteImporter::isPending(const std::string& path) const {
        const std::string assetPath = assetPathOf(path);
        if (running.count(assetPath)) return true;
        for (const Swap& s : swaps) {
            if (s.record->assetPath == assetPath) return true;
        }
        return false;
    }

    std::shared_ptr<const ImportRecord> AsepriteImporter::record(const std::string& path) const {
        const std::string assetPath = assetPathOf(path);
        std::lock_guard<std::mutex> lock(mutex);
        auto it = records.find(assetPath);
        return it == records.end() ? nullptr : it->second;
    }

    nlohmann::json AsepriteImporter::outputs(const std::string& path) const {
        std::shared_ptr<const ImportRecord> r = record(path);
        return r ? r->toJson() : nlohmann::json();
    }

    bool AsepriteImporter::resolve(const std::string& sourceRef, std::string& fileOut) const {
        const size_t hash = sourceRef.find('#');
        if (hash == std::string::npos) return false;
        const std::string source = sourceRef.substr(0, hash);
        const std::string key = sourceRef.substr(hash + 1);
        std::shared_ptr<const ImportRecord> r;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto it = records.find(source);
            if (it == records.end() || !it->second->ok) return false;
            r = it->second;
        }
        fs::path file;
        if (!r->resolveKey(key, file)) return false;
        fileOut = file.string();
        return true;
    }

    std::vector<ImportRecord::ExportedFile> ImportRecord::exportedFiles() const {
        std::vector<ExportedFile> out;
        if (!ok) return out;
        const std::string base = std::string(ImportedAssets::kImportedDir) + "/" + key.substr(0, 2) + "/" + key + "/";
        out.push_back({"sheet", sheetFile(), base + "sheet.json"});
        for (size_t p = 0; p < pages.size(); p++) {
            out.push_back({"image#" + std::to_string(p), pageFile(p), base + "page" + std::to_string(p) + ".png"});
        }
        for (const Clip& c : clips) out.push_back({c.key, dir / c.file, base + c.file});
        return out;
    }

    bool ImportRecord::resolveKey(const std::string& key, fs::path& file) const {
        if (!ok) return false;
        if (key == "sheet") {
            file = sheetFile();
            return true;
        }
        if (key.rfind("image#", 0) == 0) {
            const std::string index = key.substr(6);
            size_t page = 0;
            // (from_chars: a long run of digits fails instead of throwing)
            const auto parsed = std::from_chars(index.data(), index.data() + index.size(), page);
            if (index.empty() || parsed.ec != std::errc() || parsed.ptr != index.data() + index.size() || page >= pages.size()) return false;
            file = pageFile(page);
            return true;
        }
        if (key.rfind("animation", 0) == 0) {
            const Clip* clip = findClip(key);
            if (!clip && key.rfind("animation:", 0) == 0) clip = findClip(key.substr(10));
            if (!clip) return false;
            file = dir / clip->file;
            return true;
        }
        return false;
    }

}
