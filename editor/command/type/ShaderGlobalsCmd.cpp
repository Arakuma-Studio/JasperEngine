// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "ShaderGlobalsCmd.h"

using namespace jasperengine;

editor::ShaderGlobalsCmd::ShaderGlobalsCmd(Project* project, std::vector<ShaderGlobalDecl> before, std::vector<ShaderGlobalDecl> after,
                                           std::map<std::string, std::string> renames)
    : project(project), before(std::move(before)), after(std::move(after)), renames(std::move(renames)) {
    // a rename to itself is no rename
    for (auto it = this->renames.begin(); it != this->renames.end();) {
        if (it->first == it->second || it->first.empty() || it->second.empty())
            it = this->renames.erase(it);
        else
            ++it;
    }
}

bool editor::ShaderGlobalsCmd::apply(const std::vector<ShaderGlobalDecl>& decls){
    project->setShaderGlobals(decls);
    // the standalone build's main.cpp declares them
    project->markGeneratedSourcesDirty();
    if (project->getProjectPath().empty())
        return true;
    return project->saveProjectFile();
}

bool editor::ShaderGlobalsCmd::execute(){
    if (!renames.empty() && !editsCollected){
        // the bindings to rename, found once: undo and redo put back exactly these
        editsCollected = true;
        ShaderGlobalsUsage::forEachBindings(project, [this](const ShaderGlobalsUsage::BindingsRef& ref, const ShaderGlobalBindings& bindings) {
            ShaderGlobalBindings renamed = ShaderGlobalsUsage::renamed(bindings, renames);
            if (renamed != bindings)
                edits.push_back({ref, bindings, renamed});
        });
    }
    for (const BindingsEdit& edit : edits)
        ShaderGlobalsUsage::setBindings(project, edit.ref, edit.after);
    return apply(after);
}

void editor::ShaderGlobalsCmd::undo(){
    for (const BindingsEdit& edit : edits)
        ShaderGlobalsUsage::setBindings(project, edit.ref, edit.before);
    apply(before);
}

std::vector<editor::Command::Touched> editor::ShaderGlobalsCmd::touchedPaths(bool) const{
    if (project->getProjectPath().empty())
        return {};
    return {{project->getProjectPath() / "project.yaml", false}};
}

std::string editor::ShaderGlobalsCmd::undoRefusal() const{
    if (!renames.empty() && project->isAnyScenePlaying())
        return "Renaming shader globals is undone with Play stopped (it renames the scenes' bindings). Stop, then undo.";
    return Command::undoRefusal();
}

std::string editor::ShaderGlobalsCmd::redoRefusal() const{
    if (!renames.empty() && project->isAnyScenePlaying())
        return "Renaming shader globals is redone with Play stopped (it renames the scenes' bindings). Stop, then redo.";
    return Command::redoRefusal();
}
