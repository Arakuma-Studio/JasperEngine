// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The element selection (docs/specs/V1-vertex-colors.md R50-R52): editor state per scene, never
// saved. A named change is an undo step of its own that does not modify the scene, like a
// GameObject selection change (S24): the scene history's SelectionState names GameObjects only.

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"

#include "Project.h"
#include "command/Command.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"

#include <map>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::vertexcolor;
namespace vcd = jasperengine::editor::vertexcolor::detail;

namespace {

    std::map<uint32_t, std::vector<ElementSelection>>& vcElementSelections() {
        static std::map<uint32_t, std::vector<ElementSelection>> selections;
        return selections;
    }

    bool vcSameSelection(const std::vector<ElementSelection>& a, const std::vector<ElementSelection>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            if (a[i].entity != b[i].entity || a[i].key != b[i].key || a[i].selection.kind != b[i].selection.kind || a[i].selection.ids != b[i].selection.ids) {
                return false;
            }
        }
        return true;
    }

    class VertexColorSelectionCmd : public Command {
    public:
        VertexColorSelectionCmd(uint32_t sceneId, std::vector<ElementSelection> before, std::vector<ElementSelection> after, std::string name):
            sceneId(sceneId), before(std::move(before)), after(std::move(after)), name(std::move(name)) {}

        bool execute() override {
            vcElementSelections()[sceneId] = after;
            return true;
        }
        void undo() override { vcElementSelections()[sceneId] = before; }
        bool affectsStructure() const override { return false; }
        bool isNoOp() const override { return vcSameSelection(before, after); }
        std::string describe() const override { return name; }
        const char* typeName() const override { return "VertexColorSelectionCmd"; }
        size_t memoryBytes() const override {
            size_t bytes = sizeof(*this) + name.capacity() + (before.capacity() + after.capacity()) * sizeof(ElementSelection);
            for (const ElementSelection& item : before) bytes += item.selection.ids.capacity() * sizeof(uint32_t);
            for (const ElementSelection& item : after) bytes += item.selection.ids.capacity() * sizeof(uint32_t);
            return bytes;
        }
        bool releaseOffThread() const override { return true; }

    private:
        uint32_t sceneId;
        std::vector<ElementSelection> before;
        std::vector<ElementSelection> after;
        std::string name;
    };

}

void vcd::resetSelections() {
    vcElementSelections().clear();
}

const std::vector<ElementSelection>& editor::vertexcolor::elementSelection(uint32_t sceneId) {
    return vcElementSelections()[sceneId];
}

void editor::vertexcolor::setElementSelection(Project* project, uint32_t sceneId, std::vector<ElementSelection> selection, const std::string& undoName) {
    // one entry per target, the empty ones left out
    std::vector<ElementSelection> cleaned;
    for (ElementSelection& item : selection) {
        if (item.selection.empty()) continue;
        bool merged = false;
        for (ElementSelection& known : cleaned) {
            if (known.entity == item.entity && known.key == item.key) {
                known.selection = item.selection.kind == known.selection.kind ? vc::unite(known.selection, item.selection) : item.selection;
                merged = true;
            }
        }
        if (!merged) cleaned.push_back(std::move(item));
    }
    std::vector<ElementSelection>& current = vcElementSelections()[sceneId];
    if (undoName.empty() || !vcd::sceneProjectOf(project, sceneId)) {
        current = std::move(cleaned);
        return;
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(new VertexColorSelectionCmd(sceneId, current, std::move(cleaned), undoName));
}

void editor::vertexcolor::clearElementSelection(Project* project, uint32_t sceneId) {
    if (vcElementSelections()[sceneId].empty()) return;
    setElementSelection(project, sceneId, {}, "Deselect Mesh Elements");
}

bool editor::vertexcolor::selectionTarget(uint32_t sceneId, Entity entity, const vc::TargetKey& key, vc::Target& out) {
    for (const ElementSelection& item : vcElementSelections()[sceneId]) {
        if (item.entity != entity || item.key != key || item.selection.empty()) continue;
        out = vc::toTarget(item.selection);
        return true;
    }
    return false;
}
