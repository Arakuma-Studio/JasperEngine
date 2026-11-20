// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StringTableEditCmd.h"

#include "Out.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "io/AtomicFile.h"
#include "localization/StringTableCook.h"
#include "localization/StringTableDocument.h"
#include "util/ProjectChecks.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <map>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    std::vector<std::pair<uint64_t, StringTableEditCmd::ChangeListener>>& stringTableListeners() {
        static std::vector<std::pair<uint64_t, StringTableEditCmd::ChangeListener>> listeners;
        return listeners;
    }

    void notifyStringTableFiles(const std::vector<fs::path>& files) {
        std::vector<uint64_t> ids;
        for (const auto& item : stringTableListeners()) ids.push_back(item.first);
        for (const uint64_t id : ids) {
            const auto& listeners = stringTableListeners();
            const auto it = std::find_if(listeners.begin(), listeners.end(), [id](const auto& item) { return item.first == id; });
            if (it == listeners.end()) continue;
            const StringTableEditCmd::ChangeListener listener = it->second;
            listener(files);
        }
    }

}

uint64_t StringTableEditCmd::addChangeListener(ChangeListener listener) {
    static uint64_t nextId = 1;
    stringTableListeners().emplace_back(nextId, std::move(listener));
    return nextId++;
}

void StringTableEditCmd::removeChangeListener(uint64_t id) {
    auto& listeners = stringTableListeners();
    listeners.erase(std::remove_if(listeners.begin(), listeners.end(), [id](const auto& item) { return item.first == id; }), listeners.end());
}

StringTableEditCmd::StringTableEditCmd(std::vector<FileChange> changes, std::string label, std::string mergeKey, Project* project)
    : changes(std::move(changes)), label(std::move(label)), mergeKey(std::move(mergeKey)), project(project) {
    if (this->mergeKey.empty()) setNoMerge();
}

bool StringTableEditCmd::apply(bool forward) {
    failed.clear();
    std::vector<fs::path> written;
    for (const FileChange& change : changes) {
        const std::optional<std::string>& text = forward ? change.after : change.before;
        if (!text) {
            std::error_code ec;
            if (fs::exists(change.path, ec) && !fs::remove(change.path, ec)) {
                failed = "could not remove " + change.path.generic_string() + ": " + ec.message();
                Out::error("%s", failed.c_str());
                return false;
            }
            written.push_back(change.path);
            continue;
        }
        std::error_code ec;
        fs::create_directories(change.path.parent_path(), ec);
        // beneath the write root, as GraphEditCmd writes
        const fs::path root = rootFor(change.path);
        std::string error;
        bool ok = false;
        if (!root.empty()) {
            const std::string refused = safewrite::scopesRefusal(change.path);
            ok = refused.empty() && safewrite::writeFileResolvedBeneath(root, change.path, *text, error);
            if (!refused.empty()) error = refused;
        } else {
            const io::WriteResult result = io::writeFileAtomic(change.path, *text);
            ok = result.ok();
            error = result.message;
        }
        if (!ok) {
            failed = "could not write " + change.path.generic_string() + ": " + error;
            Out::error("%s", failed.c_str());
            return false;
        }
        written.push_back(change.path);
    }
    notifyStringTableFiles(written);
    // the editor's lookups (edit mode and Play) read the files as they are now
    l10n::refreshEditorLocalization(project);
    return true;
}

bool StringTableEditCmd::execute() {
    return apply(true);
}

void StringTableEditCmd::undo() {
    apply(false);
}

bool StringTableEditCmd::mergeWith(Command* other) {
    auto* next = dynamic_cast<StringTableEditCmd*>(other);
    if (!next || mergeKey.empty() || next->mergeKey != mergeKey) return false;
    // the first step's befores, the last step's afters
    for (const FileChange& change : next->changes) {
        auto it = std::find_if(changes.begin(), changes.end(), [&](const FileChange& c) { return c.path == change.path; });
        if (it == changes.end()) changes.push_back(change);
        else it->after = change.after;
    }
    return true;
}

bool StringTableEditCmd::isNoOp() const {
    return std::all_of(changes.begin(), changes.end(), [](const FileChange& c) { return c.before == c.after; });
}

size_t StringTableEditCmd::memoryBytes() const {
    size_t bytes = sizeof(*this);
    for (const FileChange& change : changes) bytes += (change.before ? change.before->size() : 0) + (change.after ? change.after->size() : 0);
    return bytes;
}

std::vector<Command::Touched> StringTableEditCmd::touchedPaths(bool) const {
    std::vector<Touched> out;
    for (const FileChange& change : changes) out.push_back({change.path, false});
    return out;
}

bool editor::l10n::commitSet(Project* project, const StringTableSet* before, const StringTableSet& after, const std::string& label, std::string* error,
                     const std::string& mergeKey) {
    std::map<fs::path, std::string> old = before ? before->files() : std::map<fs::path, std::string>();
    const std::map<fs::path, std::string> now = after.files();
    std::vector<StringTableEditCmd::FileChange> changes;
    for (const auto& [path, text] : now) {
        std::optional<std::string> was;
        std::string disk;
        if (ProjectChecks::readFile(path, disk)) was = disk;
        if (was != text) changes.push_back({path, was, text});
        old.erase(path);
    }
    // files of the old set the new one does not have (a removed language, a moved owner)
    for (const auto& [path, text] : old) {
        std::string disk;
        if (ProjectChecks::readFile(path, disk)) changes.push_back({path, disk, std::nullopt});
    }
    if (changes.empty()) return false;
    CommandHistory* history = project ? project->getProjectCommandHistory() : nullptr;
    auto* command = new StringTableEditCmd(std::move(changes), label, mergeKey, project);
    if (!history) {
        const bool ok = command->execute();
        if (!ok && error) *error = command->failure();
        delete command;
        return ok;
    }
    const bool ok = mergeKey.empty() ? history->addCommandNoMerge(command) : history->addCommand(command);
    if (!ok && error) *error = history->lastRefusal().empty() ? std::string("the edit could not be written") : history->lastRefusal();
    return ok;
}
