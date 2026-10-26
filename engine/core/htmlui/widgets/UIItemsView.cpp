// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIItemsView.h"
#include "UIElementInternal.h"

#include "htmlui/widgets/ItemsViewElement.h"

#include "Log.h"

#include <algorithm>

using namespace jasperengine;
using htmlui::ItemsViewElement;

namespace {

    ItemsViewElement* viewOf(const UIElement& element){
        return rmlui_dynamic_cast<ItemsViewElement*>(UIElementAccess::get(element));
    }

    const char* kindName(ItemsViewElement::Kind kind){
        switch (kind){
            case ItemsViewElement::Kind::List: return "list";
            case ItemsViewElement::Kind::Grid: return "grid";
            case ItemsViewElement::Kind::Tree: return "tree";
            case ItemsViewElement::Kind::Table: return "table";
        }
        return "list";
    }

    const char* selectionName(ItemsViewElement::SelectionMode mode){
        switch (mode){
            case ItemsViewElement::SelectionMode::None: return "none";
            case ItemsViewElement::SelectionMode::Single: return "single";
            case ItemsViewElement::SelectionMode::Multiple: return "multiple";
        }
        return "single";
    }

    ItemsViewElement::Align alignOf(const std::string& align){
        if (align == "start")
            return ItemsViewElement::Align::Start;
        if (align == "center")
            return ItemsViewElement::Align::Center;
        if (align == "end")
            return ItemsViewElement::Align::End;
        if (align != "nearest" && !align.empty())
            Log::warn("[HTML UI] scrollToIndex: unknown alignment '%s' (start, center, end, nearest); nearest is used", align.c_str());
        return ItemsViewElement::Align::Nearest;
    }

    std::vector<int> toZeroBased(const std::vector<int>& indices){
        std::vector<int> result;
        result.reserve(indices.size());
        for (int index : indices)
            result.push_back(index - 1);
        return result;
    }

    std::vector<int> toOneBased(const std::vector<int>& indices){
        std::vector<int> result;
        result.reserve(indices.size());
        for (int index : indices)
            result.push_back(index + 1);
        return result;
    }

}

#define ITEMS_VIEW(...) \
    ItemsViewElement* view = viewOf(element); \
    if (!view) \
        return __VA_ARGS__;

UIItemsView::UIItemsView() = default;

UIItemsView::UIItemsView(const UIElement& element): element(element){
}

bool UIItemsView::isValid() const{
    return viewOf(element) != nullptr;
}

UIElement UIItemsView::getElement() const{
    return isValid() ? element : UIElement();
}

std::string UIItemsView::getKind() const{
    ITEMS_VIEW(std::string())
    return kindName(view->getKind());
}

void UIItemsView::setProvider(UIItemsProvider source){
    ITEMS_VIEW()
    ItemsViewElement::Provider provider;
    provider.count = std::max(0, source.count);
    if (source.key){
        provider.key = [key = std::move(source.key)](int index){ return key(index + 1); };
    }
    if (source.height){
        provider.height = [height = std::move(source.height)](int index){ return height(index + 1); };
    }
    if (source.bind){
        provider.bind = [bind = std::move(source.bind)](Rml::Element* row, int index){ bind(UIElementAccess::wrap(row), index + 1); };
    }
    if (source.unbind){
        provider.unbind = [unbind = std::move(source.unbind)](Rml::Element* row, int index){ unbind(UIElementAccess::wrap(row), index + 1); };
    }
    provider.children = std::move(source.children);
    provider.hasChildren = std::move(source.hasChildren);
    view->setProvider(std::move(provider));
}

void UIItemsView::clearProvider(){
    ITEMS_VIEW()
    view->clearProvider();
}

int UIItemsView::getCount() const{
    ITEMS_VIEW(0)
    return view->getCount();
}

void UIItemsView::setCount(int count){
    ITEMS_VIEW()
    view->setCount(count);
}

void UIItemsView::refresh(){
    ITEMS_VIEW()
    view->refresh();
}

void UIItemsView::refreshItems(const std::vector<int>& indices){
    ITEMS_VIEW()
    view->refreshItems(toZeroBased(indices));
}

void UIItemsView::scrollToIndex(int index, const std::string& align){
    ITEMS_VIEW()
    view->scrollToIndex(index - 1, alignOf(align));
}

std::pair<int, int> UIItemsView::getVisibleRange() const{
    ITEMS_VIEW(std::make_pair(0, 0))
    const std::pair<int, int> range = view->getVisibleRange();
    return {range.first + 1, range.second + 1};
}

void UIItemsView::setSelection(const std::vector<int>& indices){
    ITEMS_VIEW()
    view->setSelection(toZeroBased(indices));
}

std::vector<int> UIItemsView::getSelection() const{
    ITEMS_VIEW(std::vector<int>())
    return toOneBased(view->getSelection());
}

std::vector<std::string> UIItemsView::getSelectedKeys() const{
    ITEMS_VIEW(std::vector<std::string>())
    return view->getSelectedKeys();
}

int UIItemsView::getActiveIndex() const{
    ITEMS_VIEW(0)
    return view->getActiveIndex() + 1;
}

void UIItemsView::setActiveIndex(int index){
    ITEMS_VIEW()
    view->setActiveIndex(index - 1);
}

void UIItemsView::activate(int index){
    ITEMS_VIEW()
    view->activate(index - 1);
}

std::string UIItemsView::keyOf(int index) const{
    ITEMS_VIEW(std::string())
    return view->keyOf(index - 1);
}

int UIItemsView::indexOfKey(const std::string& key) const{
    ITEMS_VIEW(0)
    return view->indexOfKey(key) + 1;
}

UIElement UIItemsView::getRow(int index) const{
    ITEMS_VIEW(UIElement())
    return UIElementAccess::wrap(view->rowOf(index - 1));
}

int UIItemsView::indexOf(const UIElement& row) const{
    ITEMS_VIEW(0)
    return view->itemOfElement(UIElementAccess::get(row)) + 1;
}

bool UIItemsView::expand(const std::string& key){
    ITEMS_VIEW(false)
    return view->expand(key);
}

bool UIItemsView::collapse(const std::string& key){
    ITEMS_VIEW(false)
    return view->collapse(key);
}

bool UIItemsView::toggle(const std::string& key){
    ITEMS_VIEW(false)
    return view->toggle(key);
}

bool UIItemsView::isExpanded(const std::string& key) const{
    ITEMS_VIEW(false)
    return view->isExpanded(key);
}

UITreeNode UIItemsView::getNode(int index) const{
    ITEMS_VIEW(UITreeNode())
    const ItemsViewElement::TreeNodeInfo info = view->nodeAt(index - 1);
    UITreeNode node;
    node.key = info.key;
    node.depth = info.depth;
    node.expanded = info.expanded;
    node.hasChildren = info.hasChildren;
    node.parent = info.parent + 1;
    return node;
}

std::vector<UITableColumn> UIItemsView::getColumns() const{
    ITEMS_VIEW(std::vector<UITableColumn>())
    std::vector<UITableColumn> columns;
    for (const ItemsViewElement::Column& column : view->getColumns())
        columns.push_back(UITableColumn{column.key, column.width, column.minWidth, column.sortable, column.resizable});
    return columns;
}

bool UIItemsView::setColumnWidth(const std::string& key, float width){
    ITEMS_VIEW(false)
    return view->setColumnWidth(key, width, true);
}

bool UIItemsView::sortBy(const std::string& key, const std::string& direction){
    ITEMS_VIEW(false)
    return view->sortBy(key, direction);
}

UIItemsViewState UIItemsView::getState() const{
    ITEMS_VIEW(UIItemsViewState())
    const ItemsViewElement::State state = view->getState();
    UIItemsViewState out;
    out.kind = kindName(state.kind);
    out.count = state.count;
    out.first = state.first + 1;
    out.last = state.last + 1;
    out.pool = state.pool;
    out.poolBound = state.poolBound;
    out.overscan = state.overscan;
    out.lanes = state.lanes;
    out.selection = selectionName(state.selection);
    out.selected = toOneBased(state.selected);
    out.selectedKeys = state.selectedKeys;
    out.active = state.active + 1;
    out.scrollTop = state.scrollTop;
    out.totalHeight = state.totalHeight;
    out.source = state.source;
    out.layoutBoundary = state.layoutBoundary;
    out.rowAliases = state.rowAliases;
    out.memoryBytes = state.memoryBytes;
    out.sortKey = view->getSortKey();
    out.sortDirection = view->getSortDirection();
    return out;
}

UIItemsView UIElement::getItemsView() const{
    return viewOf(*this) ? UIItemsView(*this) : UIItemsView();
}
