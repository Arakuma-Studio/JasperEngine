// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// One edit of String Table files as one step of the project history (docs/specs/game-ui-localization.md
// R28, R33, R41): every file's text before and after ("" absent: created or removed), written with
// AtomicFile under the write scope like GraphEditCmd. Edits that share a merge key (one cell's
// typing session) fold into one step. Listeners hear each changed file after execute and undo: the
// Localization Tables window, and the editor's Localization, which reloads the namespace.

#include "command/Command.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class StringTableEditCmd: public Command {
    public:
        struct FileChange {
            std::filesystem::path path;         // absolute
            std::optional<std::string> before;  // empty: the file did not exist
            std::optional<std::string> after;   // empty: the step removes it
        };
        using ChangeListener = std::function<void(const std::vector<std::filesystem::path>& files)>;

        // `project`: its editor Localization reloads after each write (may be null in tests)
        StringTableEditCmd(std::vector<FileChange> changes, std::string label, std::string mergeKey = {}, Project* project = nullptr);

        static uint64_t addChangeListener(ChangeListener listener);
        static void removeChangeListener(uint64_t id);

        bool execute() override;
        void undo() override;
        bool mergeWith(Command* other) override;
        bool affectsStructure() const override { return false; }
        std::string describe() const override { return label; }
        const char* typeName() const override { return "StringTableEditCmd"; }
        bool isNoOp() const override;
        size_t memoryBytes() const override;
        bool releaseOffThread() const override { return true; }
        std::vector<Touched> touchedPaths(bool) const override;
        std::string failure() const override { return failed; }

        const std::vector<FileChange>& getChanges() const { return changes; }

    private:
        std::vector<FileChange> changes;
        std::string label;
        std::string mergeKey;
        std::string failed;
        Project* project = nullptr;

        bool apply(bool forward);
    };

    namespace l10n {
        struct StringTableSet;

        // the step that turns `before` into `after` (both sets of one owner; an owner moved or a
        // language removed deletes its old files); false when nothing changed or the history refused
        bool commitSet(Project* project, const StringTableSet* before, const StringTableSet& after, const std::string& label,
                       std::string* error = nullptr, const std::string& mergeKey = {});
    }

}
