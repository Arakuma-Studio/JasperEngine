// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "ShaderGlobalsEdit.h"

#include "Project.h"
#include "command/type/ShaderGlobalsCmd.h"
#include "util/ShaderGlobalsUsage.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

void ShaderGlobalsEdit::begin(Project* project) {
    this->project = project;
    editRows.clear();
    for (const ShaderGlobalDecl& decl : project->getShaderGlobals()) {
        Row row;
        row.decl = decl;
        row.originalName = decl.name;
        snprintf(row.nameBuffer, sizeof(row.nameBuffer), "%s", decl.name.c_str());
        editRows.push_back(row);
    }
    previewed = false;
    committed = false;
}

void ShaderGlobalsEdit::add() {
    std::string name = "NewGlobal";
    for (int n = 1; std::any_of(editRows.begin(), editRows.end(), [&name](const Row& r) { return r.decl.name == name; }); n++)
        name = "NewGlobal" + std::to_string(n);
    Row row;
    row.decl = ShaderGlobals::makeDecl(name, ShaderGlobalType::Color);
    row.decl.defaultValue = {Vector4(1.0f, 1.0f, 1.0f, 1.0f)};
    snprintf(row.nameBuffer, sizeof(row.nameBuffer), "%s", name.c_str());
    editRows.push_back(row);
}

std::vector<ShaderGlobalDecl> ShaderGlobalsEdit::decls() const {
    std::vector<ShaderGlobalDecl> list;
    for (const Row& row : editRows) list.push_back(row.decl);
    return list;
}

std::map<std::string, std::string> ShaderGlobalsEdit::renames() const {
    std::map<std::string, std::string> renamed;
    for (const Row& row : editRows) {
        if (!row.originalName.empty() && row.originalName != row.decl.name) renamed[row.originalName] = row.decl.name;
    }
    return renamed;
}

std::string ShaderGlobalsEdit::error() const {
    const std::string invalid = ShaderGlobals::validate(decls());
    if (!invalid.empty() || !project || !project->isAnyScenePlaying())
        return invalid;
    const std::map<std::string, std::string> renamed = renames();
    if (!renamed.empty())
        return "a global cannot be renamed while playing ('" + renamed.begin()->first + "'): stop first";
    // a removed global a binding uses
    std::string bound;
    ShaderGlobalsUsage::forEachBindings(project, [&](const ShaderGlobalsUsage::BindingsRef&, const ShaderGlobalBindings& bindings) {
        for (const auto& binding : bindings) {
            const bool kept = std::any_of(editRows.begin(), editRows.end(), [&binding](const Row& r) {
                return r.originalName == binding.second || r.decl.name == binding.second;
            });
            const bool declared = std::any_of(project->getShaderGlobals().begin(), project->getShaderGlobals().end(),
                                              [&binding](const ShaderGlobalDecl& d) { return d.name == binding.second; });
            if (declared && !kept && bound.empty()) bound = binding.second;
        }
    });
    if (!bound.empty())
        return "'" + bound + "' cannot be removed while playing: bindings use it (stop first)";
    return "";
}

void ShaderGlobalsEdit::preview() {
    if (!ShaderGlobals::validate(decls()).empty())
        return;
    ShaderGlobals::declare(decls());
    previewed = true;
}

bool ShaderGlobalsEdit::commit() {
    if (!project || !error().empty())
        return false;
    committed = true;
    const std::vector<ShaderGlobalDecl> list = decls();
    const std::map<std::string, std::string> renamed = renames();
    if (list != project->getShaderGlobals() || !renamed.empty()) {
        auto* command = new ShaderGlobalsCmd(project, project->getShaderGlobals(), list, renamed);
        command->setNoMerge();
        project->getProjectCommandHistory()->addCommand(command);
    } else if (previewed) {
        project->applyShaderGlobals();
    }
    return true;
}

void ShaderGlobalsEdit::cancel() {
    if (project && previewed && !committed)
        project->applyShaderGlobals();
    previewed = false;
}
