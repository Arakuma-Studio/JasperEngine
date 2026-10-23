// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// <table-view> (S17 R11): <table-column key width min-width sortable resizable> children, whose
// inline content becomes the sticky header's cell and whose optional <template> the cell of each
// row ({{item.<key>}} in model mode without one). Dragging a header cell's 6 px edge resizes the
// column (the header and every pooled cell) and fires `columnresize`; a click on a sortable
// header fires `sort` {key, direction} and sets sorted-asc / sorted-desc. The author sorts.

#include "htmlui/widgets/ItemsViewElement.h"

#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/UIValue.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    class HeaderDragListener final : public Rml::EventListener {
    public:
        explicit HeaderDragListener(ItemsViewElement* view): view(view) {}
        void ProcessEvent(Rml::Event& event) override;
    private:
        ItemsViewElement* view;
    };

    float parseWidth(const Rml::String& text, float fallback){
        if (text.empty())
            return fallback;
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        return end == text.c_str() ? fallback : value;
    }

    // the column a header element belongs to ("" outside a header cell)
    Rml::Element* headerCellOf(Rml::Element* element){
        for (Rml::Element* each = element; each; each = each->GetParentNode()){
            if (each->IsClassSet("htmlui-header-cell"))
                return each;
            if (each->IsClassSet("htmlui-table-header"))
                return nullptr;
        }
        return nullptr;
    }

}

namespace jasperengine::htmlui {

    // the view's access for the listener (onHeaderDrag is private)
    struct TableViewAccess {
        static void drag(ItemsViewElement* view, Rml::Event& event){ view->onHeaderDrag(event); }
    };

}

void HeaderDragListener::ProcessEvent(Rml::Event& event){
    TableViewAccess::drag(view, event);
}

int ItemsViewElement::columnIndex(const std::string& key) const{
    for (size_t i = 0; i < columns.size(); i++){
        if (columns[i].key == key)
            return (int)i;
    }
    return -1;
}

void ItemsViewElement::collectColumns(){
    columns.clear();
    for (int i = 0; i < GetNumChildren(); i++){
        Rml::Element* child = GetChild(i);
        if (child->GetTagName() != "table-column")
            continue;
        Column column;
        column.key = child->GetAttribute<Rml::String>("key", "");
        if (column.key.empty())
            column.key = "column" + std::to_string(columns.size());
        column.width = std::max(1.0f, parseWidth(child->GetAttribute<Rml::String>("width", ""), 120.0f));
        column.minWidth = std::max(1.0f, parseWidth(child->GetAttribute<Rml::String>("min-width", ""), 24.0f));
        column.width = std::max(column.width, column.minWidth);
        column.sortable = child->HasAttribute("sortable");
        column.resizable = child->HasAttribute("resizable");
        for (int k = 0; k < child->GetNumChildren(); k++){
            Rml::Element* part = child->GetChild(k);
            if (part->GetTagName() == "htmltemplate" || part->GetTagName() == "template"){
                column.cellRml = templateRml(part);
                break;
            }
        }
        columns.push_back(column);
    }
}

void ItemsViewElement::buildHeader(){
    Rml::XMLAttributes attributes;
    attributes["class"] = Rml::String("htmlui-table-header");
    header = AppendChild(Rml::Factory::InstanceElement(this, "div", "div", attributes));

    std::vector<Rml::Element*> sources;
    for (int i = 0; i < GetNumChildren(); i++){
        if (GetChild(i)->GetTagName() == "table-column")
            sources.push_back(GetChild(i));
    }
    float total = 0.0f;
    for (size_t i = 0; i < columns.size() && i < sources.size(); i++){
        const Column& column = columns[i];
        Rml::XMLAttributes cellAttributes;
        cellAttributes["class"] = Rml::String(column.sortable ? "htmlui-header-cell sortable" : "htmlui-header-cell");
        cellAttributes["data-column"] = Rml::String(column.key);
        Rml::Element* cell = header->AppendChild(Rml::Factory::InstanceElement(header, "div", "div", cellAttributes));
        cell->SetProperty(Rml::PropertyId::Width, Rml::Property(column.width, Rml::Unit::PX));
        // the column's inline content moves into the cell (its elements, listeners included)
        Rml::Element* source = sources[i];
        for (int k = 0; k < source->GetNumChildren();){
            Rml::Element* part = source->GetChild(k);
            if (part->GetTagName() == "htmltemplate" || part->GetTagName() == "template"){
                k++;
                continue;
            }
            cell->AppendChild(source->RemoveChild(part));
        }
        if (column.resizable){
            Rml::XMLAttributes resizer;
            resizer["class"] = Rml::String("htmlui-column-resizer");
            cell->AppendChild(Rml::Factory::InstanceElement(cell, "div", "div", resizer));
        }
        total += column.width;
    }
    header->SetProperty(Rml::PropertyId::Width, Rml::Property(total, Rml::Unit::PX));

    headerListener = std::make_unique<HeaderDragListener>(this);
    header->AddEventListener(Rml::EventId::Dragstart, headerListener.get());
    header->AddEventListener(Rml::EventId::Drag, headerListener.get());
    header->AddEventListener(Rml::EventId::Dragend, headerListener.get());
}

void ItemsViewElement::applyColumnWidths(){
    float total = 0.0f;
    for (const Column& column : columns)
        total += column.width;
    if (header){
        header->SetProperty(Rml::PropertyId::Width, Rml::Property(total, Rml::Unit::PX));
        for (int i = 0; i < header->GetNumChildren(); i++){
            Rml::Element* cell = header->GetChild(i);
            const int index = columnIndex(cell->GetAttribute<Rml::String>("data-column", ""));
            if (index >= 0)
                cell->SetProperty(Rml::PropertyId::Width, Rml::Property(columns[index].width, Rml::Unit::PX));
        }
    }
    // every pooled cell, shown or not (they show again at their column's width)
    for (const Row& row : pool){
        if (!row.element)
            continue;
        for (int i = 0; i < row.element->GetNumChildren(); i++){
            Rml::Element* cell = row.element->GetChild(i);
            const int index = columnIndex(cell->GetAttribute<Rml::String>("data-column", ""));
            if (index >= 0)
                cell->SetProperty(Rml::PropertyId::Width, Rml::Property(columns[index].width, Rml::Unit::PX));
        }
    }
    updateSizer();
}

bool ItemsViewElement::setColumnWidth(const std::string& key, float width, bool notify){
    const int index = columnIndex(key);
    if (index < 0)
        return false;
    Column& column = columns[index];
    width = std::max(column.minWidth, width);
    if (width != column.width){
        column.width = width;
        applyColumnWidths();
    }
    if (notify){
        UIValue detail = UIValue::object();
        detail["key"] = UIValue(key);
        detail["width"] = UIValue((double)width);
        Rml::Dictionary parameters;
        parameters[HtmlUIEventNames::detailParameter] = (void*)&detail;
        DispatchEvent("columnresize", parameters, false, true);
    }
    return true;
}

bool ItemsViewElement::sortBy(const std::string& key, const std::string& direction){
    const int index = columnIndex(key);
    if (index < 0 || !columns[index].sortable)
        return false;
    std::string next = direction;
    if (next != "asc" && next != "desc")
        next = (sortKey == key && sortDirection == "asc") ? "desc" : "asc";
    sortKey = key;
    sortDirection = next;
    if (header){
        for (int i = 0; i < header->GetNumChildren(); i++){
            Rml::Element* cell = header->GetChild(i);
            const bool mine = cell->GetAttribute<Rml::String>("data-column", "") == key;
            cell->SetClass("sorted-asc", mine && next == "asc");
            cell->SetClass("sorted-desc", mine && next == "desc");
            if (mine)
                cell->SetAttribute("aria-sort", next == "asc" ? "ascending" : "descending");
            else
                cell->RemoveAttribute("aria-sort");
        }
    }
    UIValue detail = UIValue::object();
    detail["key"] = UIValue(key);
    detail["direction"] = UIValue(next);
    Rml::Dictionary parameters;
    parameters[HtmlUIEventNames::detailParameter] = (void*)&detail;
    DispatchEvent("sort", parameters, false, true);
    return true;
}

bool ItemsViewElement::handleHeaderClick(Rml::Event& event){
    Rml::Element* target = event.GetTargetElement();
    Rml::Element* cell = headerCellOf(target);
    if (!cell)
        return false;
    for (Rml::Element* each = target; each && each != cell; each = each->GetParentNode()){
        if (each->IsClassSet("htmlui-column-resizer"))
            return true;   // the end of a resize
    }
    sortBy(cell->GetAttribute<Rml::String>("data-column", ""));
    return true;
}

void ItemsViewElement::onHeaderDrag(Rml::Event& event){
    Rml::Element* target = event.GetTargetElement();
    if (!target || !target->IsClassSet("htmlui-column-resizer"))
        return;
    Rml::Element* cell = headerCellOf(target);
    const int index = cell ? columnIndex(cell->GetAttribute<Rml::String>("data-column", "")) : -1;
    if (index < 0)
        return;
    const float x = event.GetParameter<float>("mouse_x", 0.0f);
    if (event == Rml::EventId::Dragstart){
        resizingColumn = index;
        resizeStartX = x;
        resizeStartWidth = columns[index].width;
    }else if (event == Rml::EventId::Drag && resizingColumn == index){
        setColumnWidth(columns[index].key, resizeStartWidth + (x - resizeStartX));
    }else if (event == Rml::EventId::Dragend && resizingColumn == index){
        resizingColumn = -1;
        setColumnWidth(columns[index].key, resizeStartWidth + (x - resizeStartX), true);
    }
    event.StopPropagation();
}
