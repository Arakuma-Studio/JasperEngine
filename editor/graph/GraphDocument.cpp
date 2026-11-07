// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphDocument.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/GraphEditCmd.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"

#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace{

    struct GraphDocumentRegistry{
        Project* project = nullptr;
        fs::path projectPath;
        std::map<std::string, std::weak_ptr<GraphDocument>> documents;
        std::function<void(const std::vector<fs::path>&)> filesWrittenHook;
        uint64_t changeListener = 0;
    };

    GraphDocumentRegistry& documentRegistry(){
        static GraphDocumentRegistry* r = new GraphDocumentRegistry();
        return *r;
    }

    std::string documentKey(const fs::path& absolute){
        return absolute.lexically_normal().generic_string();
    }

    bool insideRoot(const fs::path& root, const fs::path& path, fs::path* relative){
        const fs::path rel = path.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || *rel.begin() == "..") return false;
        if (relative) *relative = rel;
        return true;
    }

    void useProject(Project* project){
        GraphDocumentRegistry& reg = documentRegistry();
        if (reg.project != project || reg.projectPath != project->getProjectPath()){
            reg.documents.clear();
            reg.project = project;
            reg.projectPath = project->getProjectPath();
        }
        // every write of a graph file through the history (a document's steps, edit_graph's, their
        // undo and redo) reaches the open documents and the MCP file sync
        if (reg.changeListener == 0){
            reg.changeListener = GraphEditCmd::addChangeListener([](const fs::path& file){
                GraphDocument::filesChanged({file});
                if (documentRegistry().filesWrittenHook) documentRegistry().filesWrittenHook({file});
            });
        }
    }

    bool failWith(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

}

bool GraphDocument::isGraphFile(const std::string& path){
    return graph::GraphRegistry::get().kindForFile(path) != nullptr;
}

std::shared_ptr<GraphDocument> GraphDocument::open(Project* project, const std::string& path, std::string* error){
    if (!project || project->getProjectPath().empty()){
        failWith(error, "no project is open");
        return nullptr;
    }
    if (!isGraphFile(path)){
        failWith(error, "'" + path + "' is not a file of a graph kind");
        return nullptr;
    }
    useProject(project);
    const fs::path root = project->getProjectPath().lexically_normal();
    const fs::path given = FileUtils::pathFromUtf8(path);
    fs::path absolute = given.is_absolute() ? given.lexically_normal() : (root / given).lexically_normal();
    std::error_code ec;
    if (!given.is_absolute() && !fs::exists(absolute, ec)){
        const fs::path asset = project->resolveAssetPath(given);
        if (!asset.empty() && fs::exists(asset, ec)) absolute = asset.lexically_normal();
    }
    fs::path relative;
    if (!insideRoot(root, absolute, &relative)){
        failWith(error, "'" + path + "' is outside the project");
        return nullptr;
    }
    GraphDocumentRegistry& reg = documentRegistry();
    auto it = reg.documents.find(documentKey(absolute));
    if (it != reg.documents.end()){
        if (std::shared_ptr<GraphDocument> existing = it->second.lock()) return existing;
    }
    std::string text;
    if (!ProjectChecks::readFile(absolute, text)){
        failWith(error, "cannot read '" + relative.generic_string() + "'");
        return nullptr;
    }
    auto document = std::make_shared<GraphDocument>();
    document->project = project;
    document->filePath = absolute;
    document->relativePath = relative.generic_string();
    if (!document->load(text, error)) return nullptr;
    reg.documents[documentKey(absolute)] = document;
    return document;
}

void GraphDocument::filesChanged(const std::vector<fs::path>& paths){
    GraphDocumentRegistry& reg = documentRegistry();
    for (const fs::path& path : paths){
        auto it = reg.documents.find(documentKey(path));
        if (it == reg.documents.end()) continue;
        if (std::shared_ptr<GraphDocument> document = it->second.lock()){
            document->reload();
        }else{
            reg.documents.erase(it);
        }
    }
}

void GraphDocument::filesMoved(const fs::path& oldPath, const fs::path& newPath){
    GraphDocumentRegistry& reg = documentRegistry();
    const std::string oldKey = documentKey(oldPath);
    std::vector<std::shared_ptr<GraphDocument>> moved;
    for (auto it = reg.documents.begin(); it != reg.documents.end();){
        std::shared_ptr<GraphDocument> document = it->second.lock();
        if (!document){
            it = reg.documents.erase(it);
            continue;
        }
        if (it->first == oldKey || it->first.compare(0, oldKey.size() + 1, oldKey + "/") == 0){
            moved.push_back(document);
            it = reg.documents.erase(it);
        }else{
            ++it;
        }
    }
    for (const std::shared_ptr<GraphDocument>& document : moved){
        const fs::path rest = document->filePath.lexically_relative(oldPath.lexically_normal());
        const fs::path absolute = (rest == "." ? newPath : newPath / rest).lexically_normal();
        document->filePath = absolute;
        fs::path relative;
        if (document->project && insideRoot(document->project->getProjectPath(), absolute, &relative)) document->relativePath = relative.generic_string();
        reg.documents[documentKey(absolute)] = document;
        document->notify();
    }
}

void GraphDocument::checkDiskChanges(){
    std::vector<std::shared_ptr<GraphDocument>> open;
    for (const auto& entry : documentRegistry().documents){
        if (std::shared_ptr<GraphDocument> document = entry.second.lock()) open.push_back(document);
    }
    for (const std::shared_ptr<GraphDocument>& document : open){
        std::string onDisk;
        const bool readable = ProjectChecks::readFile(document->filePath, onDisk);
        if (!readable || onDisk != document->text || !document->error.empty()) document->reload();
    }
}

void GraphDocument::closeAll(){
    documentRegistry().documents.clear();
}

void GraphDocument::setFilesWrittenHook(std::function<void(const std::vector<fs::path>&)> hook){
    documentRegistry().filesWrittenHook = std::move(hook);
}

const graph::NodeTypeDesc* GraphDocument::nodeType(const std::string& typeId) const{
    return kindDesc ? graph::GraphRegistry::nodeType(*kindDesc, typeId) : nullptr;
}

graph::Vec2 GraphDocument::displayPosition(const graph::Node& node) const{
    if (node.placed) return node.position;
    auto it = laidOut.find(node.id);
    return it != laidOut.end() ? it->second : node.position;
}

bool GraphDocument::load(const std::string& newText, std::string* loadError){
    graph::LoadReport loaded;
    graph::Graph parsed = graph::load(newText, loaded);
    const std::string problem = graph::fileProblem(relativePath, parsed, loaded);
    if (!problem.empty()){
        error = relativePath + ": " + problem;
        return failWith(loadError, error);
    }
    error.clear();
    text = newText;
    data = std::move(parsed);
    report = std::move(loaded);
    kindDesc = graph::GraphRegistry::get().kind(data.kind);
    validation = graph::validate(data);
    laidOut.clear();
    for (const auto& [id, position] : graph::layoutPositions(data, graph::unplacedNodes(data))) laidOut[id] = position;
    documentRevision++;
    return true;
}

void GraphDocument::reload(){
    std::string newText;
    if (!ProjectChecks::readFile(filePath, newText)){
        error = "'" + relativePath + "' does not exist any more";
        notify();
        return;
    }
    if (newText == text && error.empty()) return;
    std::string loadError;
    load(newText, &loadError);
    notify();
}

bool GraphDocument::apply(const std::vector<graph::GraphOp>& ops, const std::string& undoName, std::string* applyError,
                          std::vector<graph::GraphOp>* inverses){
    if (!editable()) return failWith(applyError, error.empty() ? "'" + relativePath + "' is of a graph kind this editor does not know" : error);
    if (ops.empty()) return true;
    graph::Graph copy = data;
    std::vector<std::string> names;
    size_t index = 0;
    try{
        for (const graph::GraphOp& op : ops){
            names.push_back(graph::describe(copy, op));
            graph::GraphOp inverse = graph::apply(copy, op);
            if (inverses) inverses->push_back(std::move(inverse));
            index++;
        }
    }catch (const std::exception& e){
        if (inverses) inverses->clear();
        return failWith(applyError, "op " + std::to_string(index) + " (" + ops[index].op + "): " + e.what());
    }
    const std::string after = graph::save(copy);
    if (after == text) return true;
    std::string onDisk;
    if (ProjectChecks::readFile(filePath, onDisk) && onDisk != text){
        reload();
        return failWith(applyError, "'" + relativePath + "' changed on disk since it was read: it was reloaded, make the edit again");
    }
    const std::string name = !undoName.empty() ? undoName : names.size() == 1 ? names.front() : "Edit Graph (" + std::to_string(names.size()) + " Changes)";
    if (!project->getProjectCommandHistory()->addCommand(new GraphEditCmd(filePath, text, after, name))){
        return failWith(applyError, "writing '" + relativePath + "' failed");
    }
    // (GraphEditCmd's change listener has reloaded it already; its undo and redo need that one)
    reload();
    return true;
}

int GraphDocument::addListener(std::function<void()> listener){
    const int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

void GraphDocument::removeListener(int id){
    listeners.erase(id);
}

void GraphDocument::notify(){
    // a listener may remove itself
    std::vector<std::function<void()>> calls;
    for (const auto& entry : listeners) calls.push_back(entry.second);
    for (const auto& call : calls) call();
}
