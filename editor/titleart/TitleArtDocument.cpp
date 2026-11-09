// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtDocument.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/CommandNames.h"
#include "command/type/WriteFilesCmd.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtRefs.h"
#include "TitleArtBake.h"
#include "Out.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetIndex.h"
#include "asset/Guid.h"
#include "io/watch/FileWatchService.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <fstream>
#include <optional>
#include <set>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    struct Registry {
        Project* project = nullptr;
        fs::path projectPath;
        std::map<std::string, std::weak_ptr<TitleArtDocument>> documents;
        std::function<void(const std::vector<fs::path>&)> filesWrittenHook;
        std::function<void(const std::vector<fs::path>&)> changedHook;
        std::function<void(Project*)> projectHook;
    };

    Registry& registry() {
        static Registry* r = new Registry();
        return *r;
    }

    void useProject(Project* project) {
        Registry& reg = registry();
        if (reg.project != project || reg.projectPath != project->getProjectPath()) {
            reg.documents.clear();
            reg.project = project;
            reg.projectPath = project->getProjectPath();
        }
        if (reg.projectHook) reg.projectHook(project);
    }

    bool fail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    bool isInside(const fs::path& root, const fs::path& path, fs::path* relative) {
        const fs::path rel = path.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || *rel.begin() == "..") return false;
        if (relative) *relative = rel;
        return true;
    }

    std::string key(const fs::path& absolute) {
        return absolute.lexically_normal().generic_string();
    }

    void afterWrite(const std::vector<fs::path>& changed) {
        TitleArtDocument::filesChanged(changed);
        if (registry().filesWrittenHook) registry().filesWrittenHook(changed);
        TitleArtDocument::notifyChanged(changed);
    }

    // the absolute path of a project-relative, asset-relative or absolute path
    fs::path absoluteOf(Project* project, const std::string& path, bool mustExist) {
        const fs::path root = project->getProjectPath().lexically_normal();
        const fs::path given = FileUtils::pathFromUtf8(path);
        fs::path absolute = given.is_absolute() ? given.lexically_normal() : (root / given).lexically_normal();
        std::error_code ec;
        if (mustExist && !given.is_absolute() && !fs::exists(absolute, ec)) {
            const fs::path asset = project->resolveAssetPath(given);
            if (!asset.empty() && fs::exists(asset, ec)) absolute = asset.lexically_normal();
        }
        return absolute;
    }

    TitleOpsResult refused(const titleart::TitleArtDoc& doc, const std::string& code, const std::string& message) {
        TitleOpsResult result;
        result.doc = doc;
        TitleOpProblem p;
        p.code = code;
        p.message = message;
        result.problems.push_back(std::move(p));
        return result;
    }

}

std::shared_ptr<TitleArtDocument> TitleArtDocument::open(Project* project, const std::string& path, std::string* error) {
    if (!project || project->getProjectPath().empty()) {
        fail(error, "no project is open");
        return nullptr;
    }
    if (!Util::isTitleArtFile(path)) {
        fail(error, "'" + path + "' is not a .titleart file");
        return nullptr;
    }
    useProject(project);
    const fs::path absolute = absoluteOf(project, path, true);
    fs::path relative;
    if (!isInside(project->getProjectPath(), absolute, &relative)) {
        fail(error, "'" + path + "' is outside the project");
        return nullptr;
    }
    Registry& reg = registry();
    auto it = reg.documents.find(key(absolute));
    if (it != reg.documents.end()) {
        if (std::shared_ptr<TitleArtDocument> existing = it->second.lock()) return existing;
    }
    std::string text;
    if (!ProjectChecks::readFile(absolute, text)) {
        fail(error, "cannot read '" + relative.generic_string() + "'");
        return nullptr;
    }
    auto document = std::make_shared<TitleArtDocument>();
    document->project = project;
    document->filePath = absolute;
    document->relativePath = relative.generic_string();
    document->assetRelative = project->normalizeToAssetsRelative(absolute).generic_string();
    if (!document->load(text, error)) return nullptr;
    reg.documents[key(absolute)] = document;
    return document;
}

std::shared_ptr<TitleArtDocument> TitleArtDocument::create(Project* project, const std::string& path, const titleart::TitleArtDoc* doc,
                                                           const std::string& undoName, std::string* error,
                                                        const std::vector<TitleOpFile>* files) {
    if (!project || project->getProjectPath().empty()) {
        fail(error, "no project is open");
        return nullptr;
    }
    if (!Util::isTitleArtFile(path)) {
        fail(error, "'" + path + "' does not end in .titleart");
        return nullptr;
    }
    useProject(project);
    const fs::path absolute = absoluteOf(project, path, false);
    if (!isInside(project->getProjectPath(), absolute, nullptr)) {
        fail(error, "'" + path + "' is outside the project");
        return nullptr;
    }
    std::error_code ec;
    if (fs::exists(absolute, ec)) {
        fail(error, "'" + path + "' exists already");
        return nullptr;
    }
    titleart::TitleArtDoc initial = doc ? *doc : titleart::makeNewTitleArt();
    if (!titleart::isTitleArtGuid(initial.guid())) initial.root["guid"] = titleart::generateTitleArtGuid();
    titleart::normalizeTitleArt(initial, nullptr);
    std::vector<WriteFilesCmd::Item> items = {{absolute, titleart::writeTitleArt(initial)}};
    if (files) {
        for (const TitleOpFile& f : *files) if (f.undoable && !f.remove) items.push_back({f.path, f.content});
    }
    auto* command = new WriteFilesCmd(std::move(items), &afterWrite);
    command->setName(undoName.empty() ? "Create Title Art " + CommandNames::file(absolute) : undoName);
    command->setNoMerge();
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        fail(error, "writing '" + path + "' failed" + (failure->empty() ? std::string() : " (" + *failure + ")"));
        return nullptr;
    }
    return open(project, absolute.string(), error);
}

void TitleArtDocument::setProjectHook(std::function<void(Project*)> hook) {
    registry().projectHook = std::move(hook);
}

void TitleArtDocument::filesChanged(const std::vector<fs::path>& paths, TitleArtChangeOrigin origin) {
    Registry& reg = registry();
    for (const fs::path& path : paths) {
        auto it = reg.documents.find(key(path));
        if (it == reg.documents.end()) continue;
        if (std::shared_ptr<TitleArtDocument> document = it->second.lock()) {
            document->reload(origin);
        } else {
            reg.documents.erase(it);
        }
    }
}

void TitleArtDocument::checkDiskChanges() {
    std::vector<std::shared_ptr<TitleArtDocument>> open;
    for (const auto& entry : registry().documents) {
        if (std::shared_ptr<TitleArtDocument> document = entry.second.lock()) open.push_back(document);
    }
    for (const std::shared_ptr<TitleArtDocument>& document : open) {
        std::string onDisk;
        const bool readable = ProjectChecks::readFile(document->filePath, onDisk);
        if (!readable || onDisk != document->fileText || !document->error.empty()) document->reload(TitleArtChangeOrigin::Disk);
    }
}

void TitleArtDocument::closeAll() {
    registry().documents.clear();
}

void TitleArtDocument::setFilesWrittenHook(std::function<void(const std::vector<fs::path>&)> hook) {
    registry().filesWrittenHook = std::move(hook);
}

void TitleArtDocument::setChangedHook(std::function<void(const std::vector<fs::path>&)> hook) {
    registry().changedHook = std::move(hook);
}

void TitleArtDocument::notifyChanged(const std::vector<fs::path>& paths) {
    if (registry().changedHook && !paths.empty()) registry().changedHook(paths);
}

void TitleArtDocument::filesMoved(Project* project, const fs::path& oldPath, const fs::path& newPath) {
    Registry& reg = registry();
    const std::string oldKey = key(oldPath);
    std::vector<std::shared_ptr<TitleArtDocument>> moved;
    for (auto it = reg.documents.begin(); it != reg.documents.end();) {
        std::shared_ptr<TitleArtDocument> document = it->second.lock();
        if (!document) {
            it = reg.documents.erase(it);
            continue;
        }
        if (it->first == oldKey || it->first.compare(0, oldKey.size() + 1, oldKey + "/") == 0) {
            moved.push_back(document);
            it = reg.documents.erase(it);
        } else {
            ++it;
        }
    }
    for (const std::shared_ptr<TitleArtDocument>& document : moved) {
        const fs::path rest = document->filePath.lexically_relative(oldPath.lexically_normal());
        const fs::path absolute = (rest == "." ? newPath : newPath / rest).lexically_normal();
        document->filePath = absolute;
        fs::path relative;
        if (document->project && isInside(document->project->getProjectPath(), absolute, &relative)) document->relativePath = relative.generic_string();
        if (document->project) document->assetRelative = document->project->normalizeToAssetsRelative(absolute).generic_string();
        reg.documents[key(absolute)] = document;
        document->notify(TitleArtChangeOrigin::History);
    }

    // the recipes of the derived PNGs that moved with their documents (AssetOps' pairing)
    if (!project) project = reg.project;
    std::vector<fs::path> documents;
    std::error_code ec;
    if (fs::is_regular_file(newPath, ec)) {
        if (Util::isTitleArtFile(newPath.string())) documents.push_back(newPath);
    } else if (fs::is_directory(newPath, ec)) {
        for (const fs::path& file : ProjectChecks::findProjectFiles(newPath, {titleart::TITLE_ART_EXTENSION})) documents.push_back(file);
    }
    std::vector<fs::path> rewritten;
    for (const fs::path& document : documents) {
        const std::string assetPath = project ? project->normalizeToAssetsRelative(document).generic_string() : document.filename().string();
        for (const fs::path& file : titleArtDerivedFiles(document)) {
            if (!Util::isImageFile(file.string())) continue;
            if (rewriteRecipeSource(file, assetPath)) rewritten.push_back(file);
        }
    }
    if (!rewritten.empty() && reg.filesWrittenHook) reg.filesWrittenHook(rewritten);
}

bool TitleArtDocument::rewriteRecipeSource(const fs::path& png, const std::string& documentAssetPath) {
    std::string error;
    std::optional<TitleBakeRecipe> recipe = readTitleBakeRecipe(png, &error);
    if (!recipe || recipe->sourcePath == documentAssetPath) return false;
    recipe->sourcePath = documentAssetPath;
    std::string bytes;
    if (!assetio::readFile(png, bytes)) return false;
    std::vector<uint8_t> out;
    if (!insertTitleBakeRecipe(std::vector<uint8_t>(bytes.begin(), bytes.end()), *recipe, out, error)) return false;
    ScopedExpectedWrite expected(png);
    if (!FileUtils::writeFileReplacing(png, std::string(out.begin(), out.end()))) {
        Out::warning("Could not update the recipe of %s", png.string().c_str());
        return false;
    }
    expected.reported();
    return true;
}

std::vector<fs::path> TitleArtDocument::relinkGuids(Project* project, const std::map<std::string, fs::path>& moved) {
    std::vector<fs::path> written;
    if (!project || project->getProjectPath().empty() || moved.empty()) return written;
    // the documents naming a moved GUID, from the dependency index
    std::set<std::string> referrers;
    for (const auto& entry : moved) {
        const Guid guid = Guid::parse(entry.first);
        if (guid.isNull()) continue;
        for (const DepEdge& edge : project->getAssetIndex().referencesTo(guid, std::string(), false, 4096)) {
            if (edge.kind != DepEdgeKind::TitleArtAsset) continue;
            const std::string path = project->getAssetIndex().referrerPath(edge.referrer);
            if (Util::isTitleArtFile(path)) referrers.insert(path);
        }
    }
    for (const std::string& relative : referrers) {
        const fs::path file = (project->getProjectPath() / fs::path(relative)).lexically_normal();
        std::string text;
        if (!ProjectChecks::readFile(file, text)) continue;
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        if (!parsed.ok) continue;
        bool changed = false;
        titleart::visitTitleArtRefs(parsed.doc.root, [&](titleart::Json& ref, const std::string&, titleart::TitleAssetKind) {
            const titleart::TitleAssetRef current = titleart::decodeAssetRef(ref);
            const auto found = moved.find(current.guid);
            if (current.guid.empty() || found == moved.end()) return true;
            const std::string form = project->normalizeToAssetsRelative(found->second).generic_string();
            if (fs::path(current.path).lexically_normal().generic_string() == form) return true;
            ref = titleart::encodeAssetRef({form, current.guid});
            changed = true;
            return true;
        });
        if (!changed) continue;
        const std::string newText = titleart::writeTitleArt(parsed.doc);
        if (newText == text) continue;
        if (!FileUtils::writeFileReplacing(file, newText)) {
            Out::warning("Could not update the asset paths of %s", file.string().c_str());
            continue;
        }
        written.push_back(file);
    }
    if (!written.empty()) {
        filesChanged(written);
        // the MCP file sync: rewritten by the editor, not changed outside it
        if (registry().filesWrittenHook) registry().filesWrittenHook(written);
        notifyChanged(written);
    }
    return written;
}

std::shared_ptr<TitleArtDocument> TitleArtDocument::find(const fs::path& absolute) {
    auto it = registry().documents.find(key(absolute));
    return it != registry().documents.end() ? it->second.lock() : nullptr;
}

bool TitleArtDocument::load(const std::string& newText, std::string* loadError) {
    titleart::TitleArtParseResult parsed = titleart::parseTitleArt(newText);
    if (!parsed.ok && parsed.error.code == "version_newer") {
        // opens read-only: nothing edits, renders or rebakes it
        error.clear();
        versionMessage = parsed.error.message;
        fileVersion = parsed.versionRead;
        fileText = newText;
        value = titleart::TitleArtDoc();
        loadWarnings.clear();
        appliedMigrations.clear();
        documentRevision++;
        return true;
    }
    if (!parsed.ok) {
        error = relativePath + ": " + parsed.error.toString();
        return fail(loadError, error);
    }
    error.clear();
    versionMessage.clear();
    fileVersion = parsed.versionRead;
    fileText = newText;
    value = std::move(parsed.doc);
    loadWarnings = std::move(parsed.warnings);
    appliedMigrations = std::move(parsed.migrationsApplied);
    documentRevision++;
    return true;
}

void TitleArtDocument::reload(TitleArtChangeOrigin origin) {
    std::string newText;
    if (!ProjectChecks::readFile(filePath, newText)) {
        error = "'" + relativePath + "' does not exist any more";
        notify(origin);
        return;
    }
    if (newText == fileText && error.empty()) return;
    // this document's own commit keeps the merge epoch (a drag's commits merge) and its origin;
    // anything else is an undo, a redo or an outside edit
    const bool own = !writing.empty() && newText == writing;
    if (own) origin = writingOrigin;
    writing.clear();
    std::string loadError;
    load(newText, &loadError);
    if (!own) mergeEpoch++;
    notify(origin);
}

bool TitleArtDocument::write(const std::string& newText, const std::string& undoName, const std::string& mergeKey, std::string* writeError,
                             TitleArtChangeOrigin origin) {
    std::vector<TitleOpFile> extra = std::move(pendingFiles);
    pendingFiles.clear();
    if (newText == fileText && extra.empty()) return true;
    std::string onDisk;
    if (ProjectChecks::readFile(filePath, onDisk) && onDisk != fileText) {
        reload(TitleArtChangeOrigin::Disk);
        return fail(writeError, "'" + relativePath + "' changed on disk since it was read: it was reloaded, make the edit again");
    }
    std::vector<WriteFilesCmd::Item> items = {{filePath, newText}};
    for (const TitleOpFile& f : extra) if (f.undoable && !f.remove) items.push_back({f.path, f.content});
    auto* command = new WriteFilesCmd(std::move(items), &afterWrite);
    writing = newText;
    writingOrigin = origin;
    command->setName(undoName.empty() ? "Edit Title Art " + CommandNames::file(filePath) : undoName);
    if (mergeKey.empty()) command->setNoMerge();
    else command->setMergeKey(relativePath + "\n" + std::to_string(mergeEpoch) + "\n" + mergeKey);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)) {
        writing.clear();
        return fail(writeError, "writing '" + relativePath + "' failed" + (failure->empty() ? std::string() : " (" + *failure + ")"));
    }
    writing.clear();
    return true;
}

bool TitleArtDocument::commit(const titleart::TitleArtDoc& doc, const std::string& undoName, const std::string& mergeKey, std::string* commitError,
                              TitleArtChangeOrigin origin) {
    if (!editable()) return fail(commitError, "'" + relativePath + "' is read-only: " + versionMessage);
    titleart::TitleArtDoc copy = doc;
    copy.root["guid"] = titleart::isTitleArtGuid(value.guid()) ? value.guid() : titleart::generateTitleArtGuid();
    titleart::normalizeTitleArt(copy, nullptr);
    return write(titleart::writeTitleArt(copy), undoName, mergeKey, commitError, origin);
}

bool TitleArtDocument::commitText(const std::string& text, const std::string& undoName, std::string* commitError, TitleArtChangeOrigin origin) {
    titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
    if (!parsed.ok) return fail(commitError, parsed.error.toString());
    return commit(parsed.doc, undoName.empty() ? "Restore Title Art " + CommandNames::file(filePath) : undoName, {}, commitError, origin);
}

TitleOpsResult TitleArtDocument::edit(const TitleJson& ops, const TitleArtEditOptions& options) {
    if (!editable()) return refused(value, "read_only", "'" + relativePath + "' is read-only: " + versionMessage + ". Nothing was applied.");
    if (options.expectRev >= 0 && (uint64_t)options.expectRev != documentRevision) {
        return refused(value, "rev_mismatch", "'" + relativePath + "' is at rev " + std::to_string(documentRevision) + ", not " +
                                                  std::to_string(options.expectRev) + ": read it again. Nothing was applied.");
    }
    TitleOpsResult result = TitleArtOps::apply(value, ops, options.ops);
    if (!result.ok || options.dryRun) return result;
    if (!options.undoName.empty()) result.undoName = options.undoName;
    std::string writeError;
    pendingFiles = result.files;
    if (!commit(result.doc, result.undoName, options.mergeKey, &writeError, options.origin)) {
        pendingFiles.clear();
        TitleOpsResult failed = refused(value, "write_failed", writeError + ". Nothing was applied.");
        return failed;
    }
    pendingFiles.clear();
    // the personal library lives outside the project: written once the step is in
    for (const TitleOpFile& f : result.files) {
        if (f.undoable) continue;
        std::error_code ec;
        if (f.remove) {
            fs::remove(f.path, ec);
            continue;
        }
        fs::create_directories(f.path.parent_path(), ec);
        std::ofstream out(f.path, std::ios::binary | std::ios::trunc);
        out << f.content;
        if (!out) result.problems.push_back(TitleOpProblem{"warning", "write_failed", -1, f.path.generic_string(), "could not write '" + f.path.generic_string() + "'"});
    }
    return result;
}

int TitleArtDocument::addListener(std::function<void(TitleArtChangeOrigin)> listener) {
    const int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

void TitleArtDocument::removeListener(int id) {
    listeners.erase(id);
}

void TitleArtDocument::notify(TitleArtChangeOrigin origin) {
    // a listener may remove itself
    std::vector<std::function<void(TitleArtChangeOrigin)>> calls;
    for (const auto& entry : listeners) calls.push_back(entry.second);
    for (const auto& call : calls) call(origin);
}
