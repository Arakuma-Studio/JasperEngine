// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_WIDGETS_ITEMSVIEWELEMENT_H
#define HTMLUI_WIDGETS_ITEMSVIEWELEMENT_H

// The virtualized views of HTML documents (S17 R1-R13): <list-view>, <grid-view>, <tree-view> and
// <table-view>. Only the rows that intersect the viewport (plus `overscan` rows on each side)
// exist as elements; they are pooled and recycled through VirtualWindowCore's ring, positioned
// absolutely inside a sizer as tall as every row (HeightIndex), and bound either to a data-model
// array (`items="inventory" as="item, i"`) or to a Lua provider (setProvider). The row markup is
// the view's single <template> child. Selection, keyboard and gamepad navigation, stick-to-end
// and the tree's expansion and the table's columns are the view's; the RmlUi fork's row aliases
// (P7-D) and layout boundaries (P7-L) make scrolling create no element and lay out only the view.
//
//   <list-view>                        overflow-y: auto; layout boundary (P7-L); one tab stop
//     <template>                       kept (inert): the row markup
//     <div class="htmlui-table-header">  table-view only: sticky header cells
//     <div class="htmlui-sizer">       height = HeightIndex::total()
//       <div class="htmlui-row" style="position: absolute; top: Ypx">  x pool

#include "Export.h"
#include "htmlui/widgets/HeightIndex.h"
#include "htmlui/widgets/VirtualWindowCore.h"

#include <RmlUi/Core/DataTypes.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementInstancer.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Rml {
    class DataModel;
}

namespace jasperengine::htmlui {

    class ItemsSourceView;

    class JASPERENGINE_INTERNAL ItemsViewElement : public Rml::Element, private VirtualWindowCore::Host {
    public:
        RMLUI_RTTI_DeclareWithParent(ItemsViewElement, Rml::Element)

        enum class Kind : uint8_t { List, Grid, Tree, Table };
        enum class SelectionMode : uint8_t { None, Single, Multiple };
        enum class Align : uint8_t { Start, Center, End, Nearest };

        // The Lua provider (R5), 0-based here (the Lua binding turns its 1-based indices into these).
        // key: the item's identity (selection, row identity), the index when absent; height: a
        // known height (auto views measure otherwise); bind fills a row, unbind clears it. Tree views
        // ask children(parentKey) for the keys of a node's children ("" = the roots) and bind rows by
        // flat index (nodeAt says which node it is).
        struct Provider {
            int count = 0;
            std::function<std::string(int)> key;
            std::function<float(int)> height;
            std::function<void(Rml::Element*, int)> bind;
            std::function<void(Rml::Element*, int)> unbind;
            std::function<std::vector<std::string>(const std::string&)> children;
            std::function<bool(const std::string&)> hasChildren;   // tree: children(key) is not empty when absent
        };

        // a row of a tree view's flattened, expanded structure
        struct TreeNodeInfo {
            std::string key;
            int depth = 0;
            bool expanded = false;
            bool hasChildren = false;
            int parent = -1;   // flat index of the parent, -1 for a root
        };

        // a column of a table view (<table-column>)
        struct Column {
            std::string key;
            float width = 120.0f;
            float minWidth = 24.0f;
            bool sortable = false;
            bool resizable = false;
            std::string cellRml;   // the column's <template> ("": {{item.<key>}} in model mode, empty in Lua mode)
            Rml::Element* cellTemplate = nullptr;   // that <template> (its RML is read once the document's compile is bound)
        };

        // what inspect_ui reports (R39)
        struct State {
            Kind kind = Kind::List;
            int count = 0;
            int first = -1, last = -1;          // visible items
            int pool = 0;                       // row elements (slots)
            int poolBound = 0;                  // R2: (ceil(viewport / min row height) + 2 overscan + 8) lines
            int overscan = 0;
            int lanes = 1;
            SelectionMode selection = SelectionMode::Single;
            std::vector<int> selected;          // indices, ascending
            std::vector<std::string> selectedKeys;
            int active = -1;
            float scrollTop = 0.0f;
            float totalHeight = 0.0f;
            std::string source;                 // "model:<name>", "lua", ""
            bool layoutBoundary = false;
            bool rowAliases = false;            // P7-D in use
            size_t memoryBytes = 0;             // HeightIndex + selection + tree structure
        };

        ItemsViewElement(const Rml::String& tag, Kind kind);
        ~ItemsViewElement() override;

        Kind getKind() const { return kind; }

        // --- data ---
        void setProvider(Provider provider);
        void clearProvider();
        bool hasProvider() const { return providerSet; }
        void setCount(int count);
        int getCount() const { return itemCount; }
        void refresh();                                  // every bound row again
        void refreshItems(const std::vector<int>& indices);
        // the key of an item (provider key, model key attribute, or the index / index path)
        std::string keyOf(int index) const;
        // the index of a key, -1 when no item has it (linear in the count for keyed sources)
        int indexOfKey(const std::string& key) const;

        // --- scrolling ---
        void scrollToIndex(int index, Align align);
        std::pair<int, int> getVisibleRange() const;     // first, last visible (-1, -1 when empty)

        // --- selection, activation ---
        SelectionMode getSelectionMode() const { return selectionMode; }
        void setSelection(const std::vector<int>& indices, bool notify = false);
        std::vector<int> getSelection() const;
        std::vector<std::string> getSelectedKeys() const;
        bool isSelected(int index) const;
        int getActiveIndex() const { return active; }
        // moves the active item (scrolled into view, its row focused when the view has the focus)
        void setActiveIndex(int index);
        void activate(int index);                        // the activate event

        // --- tree-view ---
        bool expand(const std::string& key);
        bool collapse(const std::string& key);
        bool toggle(const std::string& key);
        bool isExpanded(const std::string& key) const;
        TreeNodeInfo nodeAt(int index) const;

        // --- table-view ---
        const std::vector<Column>& getColumns() const { return columns; }
        bool setColumnWidth(const std::string& key, float width, bool notify = false);
        // a header click: the sort event and the sorted-asc/-desc classes
        bool sortBy(const std::string& key, const std::string& direction = std::string());
        const std::string& getSortKey() const { return sortKey; }
        const std::string& getSortDirection() const { return sortDirection; }

        // --- rows ---
        // the item a row element (or an element inside one) shows, -1 for none
        int itemOfElement(Rml::Element* element) const;
        // the row element showing an item, nullptr when it is not in the window
        Rml::Element* rowOf(int index) const;
        // where a point (document px) falls among the rows: the item and the band (-1 before, 0 on,
        // 1 after; the top and bottom 25 %), for drops (R22). False outside the rows
        bool dropTargetAt(float x, float y, int& index, int& band) const;
        // the drop indicator (.htmlui-drop-indicator) before/after/on an item, hidden for index < 0
        void showDropIndicator(int index, int band);

        State getState() const;
        // every element the views created (rows and their content), for tests
        static uint64_t rowsCreated();

        // the S17 fork features the views use (tests force both states)
        static void setForkFeatures(bool rowAliases, bool layoutBoundary);
        static void resetForkFeatures();

        // the closest view an element is in (the element itself included), nullptr for none
        static ItemsViewElement* closestView(Rml::Element* element);

        // the source data view's notification (ItemsSourceView, during the data model update)
        void onModelChanged();
        // a live patch reload bound the document to a new compile (S17 R28): the templates are read
        // again; rows of a template in `changedTemplates` (new indices) get their content anew
        void onDocumentPatched(const std::unordered_set<int>& changedTemplates);

    protected:
        void OnUpdate() override;
        void OnResize() override;
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnPropertyChange(const Rml::PropertyIdSet& changed_properties) override;
        void OnChildAdd(Rml::Element* child) override;
        void ProcessDefaultAction(Rml::Event& event) override;

    private:
        struct Row {
            Rml::Element* element = nullptr;
            int item = -1;                       // -1: unbound
            std::string key;
            int pass = 0;                        // the window update that last bound it
            uint64_t laidOutAfter = 0;           // the update whose layout first lays out its binding
            bool hidden = true;
            bool contentMade = false;            // the template's content is in the row
            std::vector<int> aliases;            // P7-D handles: item, index (tree: depth, expanded, hasChildren)
        };

        // VirtualWindowCore::Host
        void EnsurePool(int slotCount) override;
        int SlotCount() const override;
        VirtualWindowCore::Impact Rebind(int slot, int itemIndex) override;
        void UnbindSlot(int slot) override;
        bool SlotBinds(int slot, int itemIndex) const override;
        void DestroyTailSlot() override;

        // structure
        void ensureStructure();
        void readAttributes();
        std::string templateRml(Rml::Element* templateElement) const;
        void collectColumns();
        void buildHeader();
        // the markup of a row's content; the templates are read when first needed (an HTML document's
        // template content is its compile's, bound once the document has loaded)
        std::string rowContentRml();
        Rml::Element* makeRowElement();

        // data
        bool bindModel();
        void unbindModel();
        void readModelCount();
        bool modelMode() const { return model != nullptr; }
        Rml::DataAddress itemAddress(int index) const;
        void bindRowContent(Row& row, int item);
        bool makeRowContent(Row& row);
        void clearRowContent(Row& row);
        void positionRow(Row& row, int slot);
        void setRowClasses(Row& row);

        // tree
        void rebuildTree();
        void flattenChildren(int parentFlat, const std::string& parentKey, int depth, const Rml::DataAddress* parentAddress);
        Rml::DataAddress treeAddress(int index) const;

        // window
        void updateWindow(VirtualizationCoordinator::Reason reason, bool allowShrink);
        void measureRows();
        float viewportHeight() const;
        float rowsTop() const;                   // the sizer's top inside the scroll content
        // the scroll offset in the sizer's coordinates (a table's sticky header covers the rows' top),
        // and the view's scroll offset that shows a sizer y at the viewport's top
        float sizerScroll() const;
        float scrollForSizer(float y) const;
        int lineCount() const;
        void updateSizer();
        void updateBoundary();
        bool hasDefiniteSize() const;
        void checkHeight();

        // selection and keys
        void selectFromPointer(int index, bool toggle, bool extend);
        void selectRange(int from, int to, bool add);
        void notifySelection();
        bool handleKey(Rml::Event& event);
        void moveActive(int index, bool extend);
        void focusActiveRow();
        void dispatchDetail(const char* type, int index);

        // table header interaction
        bool handleHeaderClick(Rml::Event& event);
        void applyColumnWidths();
        int columnIndex(const std::string& key) const;

        Kind kind;
        bool structureReady = false;
        Rml::Element* templateElement = nullptr;
        Rml::Element* header = nullptr;
        Rml::Element* sizer = nullptr;
        Rml::Element* indicator = nullptr;
        std::string rowRml;

        // attributes
        float itemHeight = 32.0f;
        float itemWidth = 96.0f;
        float gap = 0.0f;
        float indent = 16.0f;
        bool autoHeight = false;
        float estimatedHeight = 0.0f;            // estimated-height (0: the first measured row)
        int overscan = 4;
        SelectionMode selectionMode = SelectionMode::Single;
        bool anchorEnd = false;
        std::string itemsExpression;
        std::string aliasItem = "item", aliasIndex = "i";
        std::string keyField;                    // key="id": the model items' key field
        std::string childrenKey = "children";

        // the data model source (A)
        Rml::DataModel* model = nullptr;
        Rml::DataAddress containerAddress;
        std::string modelName;
        bool modelDirty = false;

        // the Lua source (B)
        Provider provider;
        bool providerSet = false;

        int itemCount = 0;
        int lanes = 1;
        HeightIndex heights;                     // over lines (a grid's rows of cells)
        float minLineHeight = 0.0f;
        VirtualWindowCore core;
        std::vector<Row> pool;
        std::unordered_map<std::string, int> keySlots;   // key -> slot showing it
        int pass = 0;
        uint64_t updates = 0;                    // OnUpdate calls (the layout of update n follows it)
        bool inUpdate = false;
        bool inModelUpdate = false;              // onModelChanged, inside the data model's update
        bool contentPending = false;             // rows wait for a readable template
        bool rebindAll = false;
        std::vector<int> refreshSlots;
        bool windowDirty = true;
        bool measuring = false;
        float lastScrollTop = 0.0f;
        bool atEnd = false;
        bool heightChecked = false;
        bool indefiniteHeight = false;
        bool boundary = false;

        // selection
        std::unordered_set<std::string> selectedKeys;
        int active = -1;
        int anchor = -1;
        bool focusRowWanted = false;

        // pending scroll
        int pendingScrollIndex = -1;
        Align pendingAlign = Align::Nearest;

        // tree
        std::vector<TreeNodeInfo> nodes;         // flattened, expanded structure
        std::vector<std::vector<int>> nodePaths; // model mode: each node's index path
        std::unordered_set<std::string> expandedKeys;
        bool treeDirty = true;

        // table
        std::vector<Column> columns;
        std::string sortKey, sortDirection;
        int resizingColumn = -1;
        float resizeStartX = 0.0f, resizeStartWidth = 0.0f;
        std::unique_ptr<Rml::EventListener> headerListener;   // the resizers' drag events (they have no bubbling default action)
        void onHeaderDrag(Rml::Event& event);

        friend class ItemsSourceView;
        friend struct TableViewAccess;
    };

    // the instancer of the four tags
    class JASPERENGINE_INTERNAL ItemsViewInstancer : public Rml::ElementInstancer {
    public:
        Rml::ElementPtr InstanceElement(Rml::Element* parent, const Rml::String& tag, const Rml::XMLAttributes& attributes) override;
        void ReleaseElement(Rml::Element* element) override;
    };

}

#endif //HTMLUI_WIDGETS_ITEMSVIEWELEMENT_H
