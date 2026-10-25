// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIITEMSVIEW_H
#define UIITEMSVIEW_H

#include "Export.h"
#include "UIElement.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine{

    // A Lua or C++ data source of a virtual view (S17 R5). Indices are 1-based, as in Lua. key
    // names an item (selection and row identity; the index when absent), height gives a known
    // height (item-height="auto" views measure the others), bind fills a row element for an
    // item, unbind clears it before the row shows another one. A <tree-view> asks children(key)
    // for the keys of a node's children ("" = the roots) and hasChildren(key) whether it has any
    // (children(key) is not empty when absent); its bind gets the flat index (getNode says which
    // node it is).
    struct UIItemsProvider {
        int count = 0;
        std::function<std::string(int index)> key;
        std::function<float(int index)> height;
        std::function<void(const UIElement& row, int index)> bind;
        std::function<void(const UIElement& row, int index)> unbind;
        std::function<std::vector<std::string>(const std::string& parentKey)> children;
        std::function<bool(const std::string& key)> hasChildren;
    };

    // a node of a <tree-view>'s expanded structure
    struct UITreeNode {
        std::string key;
        int depth = 0;              // 0 for a root
        bool expanded = false;
        bool hasChildren = false;
        int parent = 0;             // flat index of the parent, 0 for a root
    };

    // A <table-view> column (<table-column>)
    struct UITableColumn {
        std::string key;
        float width = 0.0f;
        float minWidth = 0.0f;
        bool sortable = false;
        bool resizable = false;
    };

    // What a view shows (inspect_ui's widget state, S17 R39). first/last/active are 1-based (0: none).
    struct UIItemsViewState {
        std::string kind;           // "list", "grid", "tree", "table"
        int count = 0;
        int first = 0, last = 0;    // the visible items
        int pool = 0;               // row elements
        int poolBound = 0;          // the most rows the pool may hold (R2)
        int overscan = 0;
        int lanes = 1;              // grid columns
        std::string selection;      // "none", "single", "multiple"
        std::vector<int> selected;
        std::vector<std::string> selectedKeys;
        int active = 0;
        float scrollTop = 0.0f;
        float totalHeight = 0.0f;
        std::string source;         // "model:<name>", "lua" or ""
        bool layoutBoundary = false;   // fork P7-L in use
        bool rowAliases = false;       // fork P7-D in use
        size_t memoryBytes = 0;     // heights, keys, selection and tree structure
        std::string sortKey, sortDirection;
    };

    // The view of a <list-view>, <grid-view>, <tree-view> or <table-view> element
    // (UIElement::getItemsView, Lua el.itemsView). A value type holding a weak reference: isValid()
    // is false once the element is gone. Indices are 1-based, as in Lua; a data-model view's
    // template sees the 0-based `i` of data-for.
    class JASPERENGINE_API UIItemsView {
    public:
        UIItemsView();                              // invalid
        bool isValid() const;
        UIElement getElement() const;
        std::string getKind() const;

        // --- data ---
        void setProvider(UIItemsProvider provider);
        void clearProvider();
        int getCount() const; void setCount(int count);
        void refresh();                             // every bound row again (a provider tree: its children asked again)
        void refreshItems(const std::vector<int>& indices);

        // --- scrolling ---
        // align: "start", "center", "end", "nearest"
        void scrollToIndex(int index, const std::string& align = "nearest");
        std::pair<int, int> getVisibleRange() const;   // (0, 0) when empty

        // --- selection, activation ---
        void setSelection(const std::vector<int>& indices);
        std::vector<int> getSelection() const;
        std::vector<std::string> getSelectedKeys() const;
        int getActiveIndex() const; void setActiveIndex(int index);
        void activate(int index);                   // the activate event
        std::string keyOf(int index) const;
        int indexOfKey(const std::string& key) const;   // 0 when no item has it

        // --- rows ---
        UIElement getRow(int index) const;          // the row showing an item (invalid off screen)
        int indexOf(const UIElement& element) const;   // the item a row (or an element in one) shows, 0 for none

        // --- tree-view ---
        bool expand(const std::string& key);
        bool collapse(const std::string& key);
        bool toggle(const std::string& key);
        bool isExpanded(const std::string& key) const;
        UITreeNode getNode(int index) const;

        // --- table-view ---
        std::vector<UITableColumn> getColumns() const;
        bool setColumnWidth(const std::string& key, float width);
        // direction "asc"/"desc"; "" toggles (a header click)
        bool sortBy(const std::string& key, const std::string& direction = std::string());

        UIItemsViewState getState() const;

    private:
        friend class UIElement;
        explicit UIItemsView(const UIElement& element);
        UIElement element;
    };

}

#endif //UIITEMSVIEW_H
