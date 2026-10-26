// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PoseAnimationDocument.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "util/ProjectChecks.h"

#include <cctype>

using namespace jasperengine;

namespace fs = std::filesystem;

namespace {

    // The open documents, by absolute normalised path; they belong to `registryProject`
    struct Registry {
        editor::Project* project = nullptr;
        fs::path projectPath;
        std::map<std::string, std::shared_ptr<editor::PoseAnimationDocument>> documents;
        std::function<void(const std::vector<fs::path>&)> filesWrittenHook;
    };

    Registry& registry(){
        static Registry instance;
        return instance;
    }

    std::string keyOf(const fs::path& absolute){
        return absolute.lexically_normal().generic_string();
    }

    // Another project (or the same object reopened elsewhere) drops the documents of the old one
    void useProject(editor::Project* project){
        Registry& reg = registry();
        if (reg.project != project || reg.projectPath != project->getProjectPath()){
            reg.documents.clear();
            reg.project = project;
            reg.projectPath = project->getProjectPath();
        }
    }

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    // Project-relative (or absolute inside the project) -> absolute; false outside the project
    bool resolvePath(editor::Project* project, const std::string& path, fs::path& absolute, std::string& relative, std::string* error){
        if (!project || project->getProjectPath().empty()){
            return fail(error, "no project is open");
        }
        const fs::path root = project->getProjectPath().lexically_normal();
        fs::path given = fs::u8path(path);
        absolute = (given.is_absolute() ? given : root / given).lexically_normal();
        const fs::path rel = absolute.lexically_relative(root);
        if (rel.empty() || *rel.begin() == ".."){
            return fail(error, "'" + path + "' is outside the project");
        }
        std::string ext = absolute.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".poseanim"){
            return fail(error, "'" + path + "' is not a .poseanim file");
        }
        relative = rel.generic_string();
        return true;
    }

}

void editor::PoseAnimationDocument::setFilesWrittenHook(std::function<void(const std::vector<fs::path>&)> hook){
    registry().filesWrittenHook = std::move(hook);
}

std::string editor::PoseAnimationDocument::fileText(const pose::PoseAnimationData& data){
    pose::PoseAnimationData copy = data;
    std::vector<pose::ClampReport> reports;
    pose::clampData(copy, reports);
    return pose::writePoseAnimation(copy);
}

std::shared_ptr<editor::PoseAnimationDocument> editor::PoseAnimationDocument::open(Project* project, const std::string& path, std::string* error){
    fs::path absolute;
    std::string relative;
    if (!resolvePath(project, path, absolute, relative, error)){
        return nullptr;
    }
    useProject(project);
    Registry& reg = registry();
    const std::string key = keyOf(absolute);
    auto it = reg.documents.find(key);
    if (it != reg.documents.end()){
        std::shared_ptr<PoseAnimationDocument> document = it->second;
        // changed by something that did not tell (another program, between two polls)
        if (document->stampChanged() || !document->onDisk){
            document->reloadFromDisk();
        }
        if (!document->onDisk){
            fail(error, "'" + relative + "' does not exist");
            return nullptr;
        }
        return document;
    }

    std::shared_ptr<PoseAnimationDocument> document(new PoseAnimationDocument());
    document->project = project;
    document->relativePath = relative;
    document->filePath = absolute;
    if (!document->load(error)){
        return nullptr;
    }
    reg.documents[key] = document;
    return document;
}

std::shared_ptr<editor::PoseAnimationDocument> editor::PoseAnimationDocument::create(Project* project, const std::string& path,
        const pose::PoseAnimationData& initial, const std::string& undoName, std::vector<pose::ClampReport>* clamped,
        std::string* error, const std::vector<ExtraFile>& extraFiles){
    (void)undoName;   // the command history has no step names
    fs::path absolute;
    std::string relative;
    if (!resolvePath(project, path, absolute, relative, error)){
        return nullptr;
    }
    std::error_code ec;
    if (fs::exists(absolute, ec)){
        fail(error, "'" + relative + "' already exists");
        return nullptr;
    }
    useProject(project);
    Registry& reg = registry();
    const std::string key = keyOf(absolute);

    std::shared_ptr<PoseAnimationDocument> document;
    auto it = reg.documents.find(key);
    if (it != reg.documents.end()){
        document = it->second;   // a document whose file was deleted (an undone creation)
    }else{
        document.reset(new PoseAnimationDocument());
        document->project = project;
        document->relativePath = relative;
        document->filePath = absolute;
        reg.documents[key] = document;
    }

    pose::PoseAnimationData copy = initial;
    std::vector<pose::ClampReport> reports;
    pose::clampData(copy, reports);
    if (!document->writeUndoable(pose::writePoseAnimation(copy), "", extraFiles, error)){
        if (!document->onDisk){
            reg.documents.erase(key);
        }
        return nullptr;
    }
    if (!document->onDisk){
        // the write's notification did not reach it (no file after a successful write?)
        document->reloadFromDisk();
    }
    if (clamped){
        clamped->insert(clamped->end(), reports.begin(), reports.end());
    }
    return document;
}

bool editor::PoseAnimationDocument::load(std::string* error){
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)){
        return fail(error, "'" + relativePath + "' does not exist");
    }
    std::string fileContent;
    if (!ProjectChecks::readFile(filePath, fileContent)){
        return fail(error, "cannot read '" + relativePath + "'");
    }
    pose::PoseAnimationData parsed;
    std::vector<std::string> parseWarnings;
    std::string parseError;
    if (!pose::parsePoseAnimation(fileContent, parsed, parseWarnings, &parseError)){
        return fail(error, "'" + relativePath + "' is not a pose animation: " + parseError);
    }
    current = std::move(parsed);
    warnings = std::move(parseWarnings);
    text = std::move(fileContent);
    onDisk = true;
    rev++;
    takeStamp();
    return true;
}

void editor::PoseAnimationDocument::takeStamp(){
    std::error_code ec;
    const auto time = fs::last_write_time(filePath, ec);
    stampTime = ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
    const auto size = fs::file_size(filePath, ec);
    stampSize = ec ? 0 : static_cast<uint64_t>(size);
}

bool editor::PoseAnimationDocument::stampChanged() const{
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)){
        return onDisk;
    }
    const auto time = fs::last_write_time(filePath, ec);
    if (ec) return false;
    const auto size = fs::file_size(filePath, ec);
    if (ec) return false;
    return !onDisk || static_cast<int64_t>(time.time_since_epoch().count()) != stampTime || static_cast<uint64_t>(size) != stampSize;
}

void editor::PoseAnimationDocument::reloadFromDisk(){
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)){
        if (onDisk){
            onDisk = false;
            text.clear();
            rev++;
            notify(Change::Removed);
        }
        return;
    }
    std::string fileContent;
    if (!ProjectChecks::readFile(filePath, fileContent)){
        return;   // being written: the next poll or notification reads it
    }
    if (onDisk && fileContent == text){
        takeStamp();
        return;
    }
    pose::PoseAnimationData parsed;
    std::vector<std::string> parseWarnings;
    std::string parseError;
    if (!pose::parsePoseAnimation(fileContent, parsed, parseWarnings, &parseError)){
        // keep the last good data; an edit through commit() replaces the broken file
        warnings = {"the file on disk is not a valid pose animation (" + parseError + "): showing its last valid content"};
        text = std::move(fileContent);
        onDisk = true;
        takeStamp();
        notify(Change::Reloaded);
        return;
    }
    current = std::move(parsed);
    warnings = std::move(parseWarnings);
    text = std::move(fileContent);
    onDisk = true;
    rev++;
    takeStamp();
    notify(committing ? Change::Edited : Change::Reloaded);
}

bool editor::PoseAnimationDocument::writeUndoable(const std::string& newText, const std::string& mergeKey,
                                                  const std::vector<ExtraFile>& extraFiles, std::string* error){
    std::vector<WriteFilesCmd::Item> items;
    if (!onDisk || newText != text){
        items.push_back({filePath, newText});
    }
    for (const ExtraFile& extra : extraFiles){
        std::string existing;
        std::error_code ec;
        if (fs::is_regular_file(extra.path, ec) && ProjectChecks::readFile(extra.path, existing) && existing == extra.content){
            continue;
        }
        if (fs::is_directory(extra.path, ec)){
            return fail(error, extra.path.generic_string() + " is a directory");
        }
        items.push_back({extra.path, extra.content});
    }
    if (items.empty()){
        return true;
    }
    auto* command = new WriteFilesCmd(std::move(items), [](const std::vector<fs::path>& changed){
        PoseAnimationDocument::filesChanged(changed);
        if (registry().filesWrittenHook){
            registry().filesWrittenHook(changed);
        }
    });
    if (mergeKey.empty()){
        command->setNoMerge();
    }else{
        command->setMergeKey(relativePath + "\n" + mergeKey);
    }
    committing = true;
    // addCommand runs execute() (which reloads this document) and deletes the command when it fails
    const bool written = project->getProjectCommandHistory()->addCommand(command);
    committing = false;
    if (!written){
        return fail(error, "writing '" + relativePath + "' failed (permissions, a read-only disk, or a parent path that is a file)");
    }
    return true;
}

bool editor::PoseAnimationDocument::commit(const pose::PoseAnimationData& newData, const std::string& undoName,
                                           const std::string& mergeKey, std::vector<pose::ClampReport>* clamped,
                                           std::string* error, const std::vector<ExtraFile>& extraFiles){
    (void)undoName;   // the command history has no step names
    if (!onDisk){
        return fail(error, "'" + relativePath + "' does not exist any more");
    }
    // written by something else since it was read (the poll or auto-sync had not seen it yet):
    // the edit was made on old content, so it is not written over the new one
    if (stampChanged()){
        reloadFromDisk();
        return fail(error, "'" + relativePath + "' changed on disk since it was read: it was reloaded, make the edit again");
    }
    pose::PoseAnimationData copy = newData;
    std::vector<pose::ClampReport> reports;
    pose::clampData(copy, reports);
    if (!writeUndoable(pose::writePoseAnimation(copy), mergeKey, extraFiles, error)){
        return false;
    }
    if (clamped){
        clamped->insert(clamped->end(), reports.begin(), reports.end());
    }
    return true;
}

void editor::PoseAnimationDocument::filesChanged(const std::vector<fs::path>& paths){
    Registry& reg = registry();
    std::vector<std::shared_ptr<PoseAnimationDocument>> changed;
    for (const fs::path& path : paths){
        auto it = reg.documents.find(keyOf(path));
        if (it != reg.documents.end()){
            changed.push_back(it->second);
        }
    }
    for (const std::shared_ptr<PoseAnimationDocument>& document : changed){
        document->reloadFromDisk();
    }
}

void editor::PoseAnimationDocument::checkDiskChanges(){
    std::vector<std::shared_ptr<PoseAnimationDocument>> documents;
    for (const auto& entry : registry().documents){
        if (entry.second->stampChanged()){
            documents.push_back(entry.second);
        }
    }
    for (const std::shared_ptr<PoseAnimationDocument>& document : documents){
        document->reloadFromDisk();
    }
}

int editor::PoseAnimationDocument::addListener(Listener listener){
    const int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

void editor::PoseAnimationDocument::removeListener(int id){
    listeners.erase(id);
}

void editor::PoseAnimationDocument::notify(Change change){
    // a listener may add or remove listeners
    std::map<int, Listener> copy = listeners;
    for (auto& entry : copy){
        if (listeners.count(entry.first) && entry.second){
            entry.second(*this, change);
        }
    }
}

std::shared_ptr<editor::PoseRealisation> editor::PoseAnimationDocument::realiseData(const pose::PoseAnimationData& data,
                                                                                  const std::shared_ptr<const pose::PoseRig>& rig){
    return pose::realisePoseAnimation(data, rig);
}

void editor::PoseAnimationDocument::closeAll(){
    Registry& reg = registry();
    reg.documents.clear();
    reg.project = nullptr;
    reg.projectPath.clear();
}

std::shared_ptr<const editor::PoseRealisation> editor::PoseAnimationDocument::realise(const std::shared_ptr<const pose::PoseRig>& rig){
    if (!rig){
        return nullptr;
    }
    if (realised && realised->rig == rig && realised->revision == rev){
        return realised;
    }
    std::shared_ptr<PoseRealisation> made = realiseData(current, rig);
    if (made){
        made->revision = rev;
    }
    realised = made;
    return realised;
}
