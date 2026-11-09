// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R1, R2, R7, R9, R54, §6.8)
// - Purpose: the editor's working copy of one `.titleart`, shared per path by everything that
//   edits or shows it (the Title Designer, the Inspector, MCP's edit_title, editor Lua): the
//   AnimatorAssetDocument design. Every edit is a commit: the document is normalized, written as
//   its canonical text through the project command history (WriteFilesCmd: one named undo step;
//   a non-empty merge key merges drags) and read back, so doc() is what the file holds. Undo,
//   redo and outside changes reach documents through filesChanged(), which reloads them and
//   notifies listeners with the origin.
// - edit() is the one entry of op batches (TitleArtOps): all or nothing, `expect_rev` refuses
//   when the file moved on, `dry_run` validates without writing; a batch is one undo step named
//   by its ops.
// - revision() is the `rev` agents see: it rises with every new text the document holds.
// - Versions: an older file loads migrated in memory and is rewritten in the current version
//   only by the next commit; a newer one opens read-only (editable() false, versionError() says
//   which version wrote it): commits are refused and nothing renders or rebakes it.
// - Thread-safety: main thread.
// - Invariants: one document per normalized absolute path; a file that stops parsing keeps the
//   last good value and reports the error; the GUID of an existing file never changes on commit.

#pragma once

#include "titleart/TitleArtDoc.h"
#include "TitleArtOps.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    // who made a document change (S82's history and MCP's external_changes tell them apart)
    enum class TitleArtChangeOrigin : uint8_t { Person, Agent, History, Disk };

    struct TitleArtEditOptions {
        std::string undoName;           // empty: the name TitleArtOps gives the batch
        std::string mergeKey;           // non-empty: merges with the previous step of the same key (drags)
        int64_t expectRev = -1;         // >= 0: refused (rev_mismatch) unless revision() equals it
        bool dryRun = false;            // validate only: nothing is written
        TitleArtChangeOrigin origin = TitleArtChangeOrigin::Person;
        TitleOpsOptions ops;
    };

    class TitleArtDocument {
    public:
        // the document of a path (absolute, project-relative or asset-relative); null + `error`
        // when it is not a .titleart, is outside the project, or does not load (missing, not JSON,
        // not a Title Art). A newer version opens read-only.
        static std::shared_ptr<TitleArtDocument> open(Project* project, const std::string& path, std::string* error);
        // a new file (refused when it exists) holding `doc` (makeNewTitleArt's when null; its GUID
        // kept when it has a valid one), as one project undo step named `undoName`; `files` (S87's imports: fonts, images,
        // split SVGs) are written in the same step
        static std::shared_ptr<TitleArtDocument> create(Project* project, const std::string& path, const titleart::TitleArtDoc* doc,
                                                        const std::string& undoName, std::string* error,
                                                        const std::vector<TitleOpFile>& files = {});
        // reload the documents of these files; listeners notified with `origin` (the history's
        // undo and redo by default; a hot reload of an outside edit passes Disk)
        static void filesChanged(const std::vector<std::filesystem::path>& paths, TitleArtChangeOrigin origin = TitleArtChangeOrigin::History);
        // every open document whose file differs from what it read reloads (outside edits)
        static void checkDiskChanges();
        static void closeAll();
        // called with the files a commit (or its undo / redo) wrote: the MCP file sync
        static void setFilesWrittenHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);
        // the open document of an absolute path, null when none
        static std::shared_ptr<TitleArtDocument> find(const std::filesystem::path& absolute);
        // called with every document file a commit, an undo or a redo wrote, and with every
        // document an outside edit reloaded (Auto Bake, previews and thumbnails)
        static void setChangedHook(std::function<void(const std::vector<std::filesystem::path>&)> hook);
        static void notifyChanged(const std::vector<std::filesystem::path>& paths);

        // R5: a file or directory moved (AssetOps::move): open documents at or under `oldPath`
        // follow it, and the recipes of the derived PNGs of every document now at or under
        // `newPath` name its new path (their source GUID is unchanged)
        static void filesMoved(Project* project, const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        // R5: assets moved (GUID -> new absolute path): every document naming one of them by GUID
        // (the dependency index) gets that reference's `path` hint rewritten; the rewritten files
        static std::vector<std::filesystem::path> relinkGuids(Project* project, const std::map<std::string, std::filesystem::path>& moved);
        // a baked PNG's recipe rewritten to name `documentAssetPath` (nothing when it already does)
        static bool rewriteRecipeSource(const std::filesystem::path& png, const std::string& documentAssetPath);

        const std::filesystem::path& absolutePath() const { return filePath; }
        const std::string& projectPath() const { return relativePath; }
        const std::string& assetPath() const { return assetRelative; }
        uint64_t revision() const { return documentRevision; }
        const titleart::TitleArtDoc& doc() const { return value; }
        const std::string& text() const { return fileText; }
        const std::string& guid() const { return value.guid(); }
        const std::vector<titleart::TitleWarning>& warnings() const { return loadWarnings; }
        // the last reload's error (the file stopped parsing); empty when it holds `doc`
        const std::string& loadError() const { return error; }
        // false for a file of a newer version (versionError() says which)
        bool editable() const { return versionMessage.empty(); }
        const std::string& versionError() const { return versionMessage; }
        int versionRead() const { return fileVersion; }
        // the file is an older version (the next commit writes the current one)
        bool migrated() const { return !appliedMigrations.empty(); }

        // writes `doc` (normalized; this document's GUID kept) as one undo step named `undoName`;
        // false + `error` when it is read-only, cannot be written or changed on disk meanwhile
        // `files` are written in the same undo step (an op batch's TitleOpsResult::files)
        bool commit(const titleart::TitleArtDoc& doc, const std::string& undoName, const std::string& mergeKey, std::string* error,
                    TitleArtChangeOrigin origin = TitleArtChangeOrigin::Person, const std::vector<TitleOpFile>& files = {});
        // a whole-document text (S82's Restore This Version): parsed, normalized, this document's
        // GUID kept, one undo step
        bool commitText(const std::string& text, const std::string& undoName, std::string* error,
                        TitleArtChangeOrigin origin = TitleArtChangeOrigin::Person);
        // an op batch (TitleArtOps::apply) committed as one undo step; the result says why it was
        // refused (`rev_mismatch`, `read_only`, `write_failed`, or the batch's own problems)
        TitleOpsResult edit(const TitleJson& ops, const TitleArtEditOptions& options = {});
        // reads the file again (listeners notified with `origin` when it changed; a commit of this
        // document keeps its own origin)
        void reload(TitleArtChangeOrigin origin = TitleArtChangeOrigin::Disk);

        int addListener(std::function<void(TitleArtChangeOrigin)> listener);
        void removeListener(int id);

    private:
        Project* project = nullptr;
        std::filesystem::path filePath;
        std::string relativePath;
        std::string assetRelative;
        std::string fileText;
        std::string error;
        std::string versionMessage;
        int fileVersion = 0;
        std::vector<std::string> appliedMigrations;
        uint64_t documentRevision = 0;
        std::vector<titleart::TitleWarning> loadWarnings;
        titleart::TitleArtDoc value;
        std::map<int, std::function<void(TitleArtChangeOrigin)>> listeners;
        int nextListener = 1;
        uint64_t mergeEpoch = 0;
        std::string writing;            // the text a commit of this document is writing
        TitleArtChangeOrigin writingOrigin = TitleArtChangeOrigin::Person;

        bool load(const std::string& newText, std::string* error);
        bool write(const std::string& newText, const std::string& undoName, const std::string& mergeKey, std::string* error,
                   TitleArtChangeOrigin origin, const std::vector<TitleOpFile>& files = {});
        void notify(TitleArtChangeOrigin origin);
    };

}
