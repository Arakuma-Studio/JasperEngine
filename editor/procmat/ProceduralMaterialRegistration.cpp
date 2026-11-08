// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What only the editor has: the node library embedded in its binary, and the kind registered with it.
// In a source of its own because its generated header is the size of the library.

#include "NodeLibrary.h"
#include "ProceduralMaterialKind.h"

#include "Out.h"
#include "procmat_nodes.h"

using namespace jasperengine;
using namespace jasperengine::editor::procmat;

std::shared_ptr<const NodeLibrary> NodeLibrary::shipped() {
    static const std::shared_ptr<const NodeLibrary> library = []() {
        std::vector<std::pair<std::string, std::string>> files;
        for (const ProcMatEmbeddedNode* node = procmat_embedded_nodes; node->name; node++) {
            files.emplace_back(node->name, std::string(reinterpret_cast<const char*>(node->data), node->size));
        }
        std::string error;
        std::shared_ptr<const NodeLibrary> parsed = fromFiles(files, error);
        if (!parsed) Out::error("The Procedural Material node library does not load: %s", error.c_str());
        return parsed;
    }();
    return library;
}

void jasperengine::editor::procmat::registerProceduralMaterialKind() {
    const std::shared_ptr<const NodeLibrary> library = NodeLibrary::shipped();
    if (library) graph::GraphRegistry::get().registerKind(makeProceduralMaterialKind(library));
}
