// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The editor's built-in commands: ids, groups, names, contexts and default chords (S25 §6.3),
// in catalog order. Every key the editor bound before the catalog is here with the binding and
// the context it had. The actions (run, enabled, keyGate) are attached by App
// (AppCommands.cpp), so tests load the same table into a catalog of their own.

#include "CommandCatalog.h"

#include <vector>

namespace jasperengine::editor {

    std::vector<CommandDef> defaultCommands();

    // The catalog id of a GameObject create entry (HierarchyWindow::getCreateMenuEntries):
    // its own (gameobject.createEmpty), else gameobject.create.<submenu>.<label> in camel case
    std::string createEntryCommandId(size_t entryIndex);

}
