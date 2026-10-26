// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The virtual views (S17 R1-R13): pool, window, data sources, selection and keys. Tree flattening
// is in TreeView.cpp, the table's columns and header in TableView.cpp.

#include "htmlui/widgets/ItemsViewElement.h"

#include "htmlui/widgets/DragDropSession.h"
#include "htmlui/widgets/WidgetsRuntime.h"
#include "htmlui/compat/ForkFeatures.h"
#include "htmlui/compat/LuaHooks.h"
#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/UIValue.h"

#include "Log.h"

#include "../../../libs/rmlui/Source/Core/DataModel.h"
#include "../../../libs/rmlui/Source/Core/DataView.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataVariable.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::htmlui;

RMLUI_RTTI_Define(ItemsViewElement)

namespace jasperengine::htmlui {

    // A view's notification from its data model (R4): a data view on the view element whose
    // variable is the items' top-level name, so dirtying it (model:set("inventory[3].name", v)
    // too) tells the view in the data model's update, before the elements update.
    class ItemsSourceView final : public Rml::DataView {
    public:
        ItemsSourceView(ItemsViewElement* view, Rml::String variable): Rml::DataView(view, -1), variable(std::move(variable)) {}

        bool Initialize(Rml::DataModel&, Rml::Element*, const Rml::String&, const Rml::String&) override { return true; }

        bool Update(Rml::DataModel&) override{
            if (auto* view = rmlui_dynamic_cast<ItemsViewElement*>(GetElement()))
                view->onModelChanged();
            return false;
        }

        Rml::StringList GetVariableNameList() const override { return Rml::StringList{variable}; }

        void Release() override { delete this; }

    private:
        Rml::String variable;
    };

}

namespace {

    struct ViewFeatures {
        bool forced = false;
        bool rowAliases = false;
        bool layoutBoundary = false;
    };

    ViewFeatures& viewFeatures(){
        static ViewFeatures features;
        return features;
    }

    bool useRowAliases(){
        const ViewFeatures& features = viewFeatures();
        if (features.forced)
            return features.rowAliases;
        static const bool present = compat::ForkFeatures::present().has(compat::ForkFeature::RowAliases);
        return present;
    }

    bool useLayoutBoundary(){
        const ViewFeatures& features = viewFeatures();
        if (features.forced)
            return features.layoutBoundary;
        static const bool present = compat::ForkFeatures::present().has(compat::ForkFeature::LayoutBoundary);
        return present;
    }

    uint64_t& createdCounter(){
        static uint64_t created = 0;
        return created;
    }

    // a view without a definite height lays out this many rows (R13)
    constexpr int kIndefiniteRows = 20;
    // the slots past the window a shrinking view keeps (VirtualWindowCore's slack)
    constexpr float kMinLineHeight = 1.0f;

    float parseLength(const Rml::String& text, float fallback){
        if (text.empty())
            return fallback;
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(value))
            return fallback;
        return value;
    }

    std::string trim(const std::string& text){
        const size_t start = text.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return std::string();
        const size_t end = text.find_last_not_of(" \t\r\n");
        return text.substr(start, end - start + 1);
    }

    std::string variantText(const Rml::Variant& value){
        switch (value.GetType()){
            case Rml::Variant::STRING: return value.GetReference<Rml::String>();
            case Rml::Variant::DOUBLE:
            case Rml::Variant::FLOAT:{
                const double number = value.Get<double>();
                if (std::floor(number) == number && std::abs(number) < 1e15)
                    return std::to_string((long long)number);
                return Rml::ToString(number);
            }
            default: return value.Get<Rml::String>();
        }
    }

    Rml::Property px(float value){
        return Rml::Property(value, Rml::Unit::PX);
    }

    bool isRowElement(const Rml::Element* element){
        return element && element->IsClassSet("htmlui-row");
    }

}

// ---------------------------------------------------------------- lifetime

ItemsViewElement::ItemsViewElement(const Rml::String& tag, Kind kind): Rml::Element(tag), kind(kind){
    if (kind == Kind::Grid)
        itemHeight = 96.0f;
}

ItemsViewElement::~ItemsViewElement(){
    // the provider's callbacks may hold Lua references: dropped with the view
    provider = Provider();
    // the header outlives the listener it holds (children go after this destructor)
    if (header && headerListener){
        header->RemoveEventListener(Rml::EventId::Dragstart, headerListener.get());
        header->RemoveEventListener(Rml::EventId::Drag, headerListener.get());
        header->RemoveEventListener(Rml::EventId::Dragend, headerListener.get());
    }
}

void ItemsViewElement::setForkFeatures(bool rowAliases, bool layoutBoundary){
    viewFeatures() = ViewFeatures{true, rowAliases, layoutBoundary};
}

void ItemsViewElement::resetForkFeatures(){
    viewFeatures() = ViewFeatures();
}

uint64_t ItemsViewElement::rowsCreated(){
    return createdCounter();
}

ItemsViewElement* ItemsViewElement::closestView(Rml::Element* element){
    for (Rml::Element* each = element; each; each = each->GetParentNode()){
        if (auto* view = rmlui_dynamic_cast<ItemsViewElement*>(each))
            return view;
    }
    return nullptr;
}

// ---------------------------------------------------------------- structure

void ItemsViewElement::readAttributes(){
    const Rml::String height = GetAttribute<Rml::String>("item-height", "");
    autoHeight = (height == "auto");
    if (!autoHeight)
        itemHeight = std::max(kMinLineHeight, parseLength(height, kind == Kind::Grid ? 96.0f : 32.0f));
    estimatedHeight = std::max(0.0f, parseLength(GetAttribute<Rml::String>("estimated-height", ""), 0.0f));
    itemWidth = std::max(kMinLineHeight, parseLength(GetAttribute<Rml::String>("item-width", ""), 96.0f));
    gap = std::max(0.0f, parseLength(GetAttribute<Rml::String>("gap", ""), 0.0f));
    indent = std::max(0.0f, parseLength(GetAttribute<Rml::String>("indent", ""), 16.0f));
    overscan = std::clamp((int)parseLength(GetAttribute<Rml::String>("overscan", ""), 4.0f), 0, 64);
    const Rml::String selection = GetAttribute<Rml::String>("selection", "single");
    selectionMode = selection == "none" ? SelectionMode::None : (selection == "multiple" ? SelectionMode::Multiple : SelectionMode::Single);
    anchorEnd = GetAttribute<Rml::String>("anchor", "") == "end";
    keyField = GetAttribute<Rml::String>("key", "");
    childrenKey = GetAttribute<Rml::String>("children-key", "children");
    if (childrenKey.empty())
        childrenKey = "children";

    itemsExpression = trim(GetAttribute<Rml::String>("items", ""));
    aliasItem = "item";
    aliasIndex = "i";
    const std::string as = GetAttribute<Rml::String>("as", "");
    if (!as.empty()){
        const size_t comma = as.find(',');
        const std::string first = trim(as.substr(0, comma));
        if (!first.empty())
            aliasItem = first;
        if (comma != std::string::npos){
            const std::string second = trim(as.substr(comma + 1));
            if (!second.empty())
                aliasIndex = second;
        }
    }
}

std::string ItemsViewElement::templateRml(Rml::Element* element) const{
    if (!element)
        return std::string();
    // an HTML document's <template> (compat keeps its content with the compile)
    if (element->HasAttribute(compat::LuaHooks::kTemplateAttribute))
        return compat::LuaHooks::templateContent(element);
    // an RML document's inert <template> (fork P7-T)
    return element->GetAttribute<Rml::String>("rmlui-inner-rml", "");
}

void ItemsViewElement::ensureStructure(){
    if (structureReady)
        return;
    structureReady = true;
    readAttributes();

    // the single <template> child (R1: none or several is HTMLUI-HTML-0030, reported by the compile)
    for (int i = 0; i < GetNumChildren(); i++){
        Rml::Element* child = GetChild(i);
        const Rml::String& tag = child->GetTagName();
        if (tag == "htmltemplate" || tag == "template"){
            templateElement = child;
            break;
        }
    }
    if (kind == Kind::Table){
        collectColumns();
        buildHeader();
    }

    Rml::XMLAttributes attributes;
    attributes["class"] = Rml::String("htmlui-sizer");
    Rml::ElementPtr sizerPtr = Rml::Factory::InstanceElement(this, "div", "div", attributes);
    sizer = AppendChild(std::move(sizerPtr));
    // the rows' containing block, as tall as every row
    sizer->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::Block));
    sizer->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Relative));
    attributes["class"] = Rml::String("htmlui-drop-indicator");
    indicator = sizer->AppendChild(Rml::Factory::InstanceElement(sizer, "div", "div", attributes));
    indicator->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::None));
    indicator->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Absolute));

    heights.reset(0, autoHeight ? (estimatedHeight > 0.0f ? estimatedHeight : 32.0f) : itemHeight + (kind == Kind::Grid ? gap : 0.0f));
    minLineHeight = heights.estimate();
    windowDirty = true;
}

std::string ItemsViewElement::rowContentRml(){
    if (rowRml.empty() && templateElement)
        rowRml = templateRml(templateElement);
    if (kind != Kind::Table)
        return rowRml;
    // a table row: one cell per column (none while a column's template is not readable yet)
    std::string rml;
    for (Column& column : columns){
        if (column.cellRml.empty() && column.cellTemplate){
            column.cellRml = templateRml(column.cellTemplate);
            if (column.cellRml.empty())
                return std::string();
        }
        rml += "<div class=\"htmlui-cell\" data-column=\"";
        rml += Rml::StringUtilities::EncodeRml(column.key);
        rml += "\" style=\"width: " + Rml::ToString(column.width) + "px\">";
        if (!column.cellRml.empty())
            rml += column.cellRml;
        else if (model)
            rml += "{{" + aliasItem + "." + column.key + "}}";
        rml += "</div>";
    }
    return rml;
}

Rml::Element* ItemsViewElement::makeRowElement(){
    Rml::XMLAttributes attributes;
    attributes["class"] = Rml::String("htmlui-row");
    Rml::ElementPtr row = Rml::Factory::InstanceElement(sizer, "div", "div", attributes);
    Rml::Element* element = sizer->InsertBefore(std::move(row), indicator);
    // placed in the sizer by the view (positionRow): absolutely, full width but in a grid
    element->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::None));
    element->SetProperty(Rml::PropertyId::Position, Rml::Property(Rml::Style::Position::Absolute));
    element->SetProperty(Rml::PropertyId::Left, px(0.0f));
    if (kind != Kind::Grid && kind != Kind::Table)
        element->SetProperty(Rml::PropertyId::Right, px(0.0f));
    createdCounter()++;
    return element;
}

void ItemsViewElement::OnChildAdd(Rml::Element* child){
    Rml::Element::OnChildAdd(child);
    // a template added after the view was built (a script): the rows use it from now on
    if (structureReady && child && child->GetParentNode() == this && !templateElement &&
        (child->GetTagName() == "htmltemplate" || child->GetTagName() == "template")){
        templateElement = child;
        rowRml = templateRml(child);
        refresh();
    }
}

void ItemsViewElement::OnAttributeChange(const Rml::ElementAttributes& changed){
    Rml::Element::OnAttributeChange(changed);
    if (!structureReady)
        return;
    static const char* const layoutAttributes[] = {"item-height", "estimated-height", "item-width", "gap", "overscan", "indent",
                                                   "selection", "anchor", "key", "children-key"};
    bool relayout = false;
    for (const char* name : layoutAttributes)
        relayout |= changed.count(name) > 0;
    if (changed.count("items") || changed.count("as")){
        unbindModel();
        relayout = true;
    }
    if (!relayout)
        return;
    readAttributes();
    heights.reset(0, autoHeight ? (estimatedHeight > 0.0f ? estimatedHeight : 32.0f) : itemHeight + (kind == Kind::Grid ? gap : 0.0f));
    minLineHeight = heights.estimate();
    treeDirty = true;
    rebindAll = true;
    windowDirty = true;
    core.Reset();
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0);
}

void ItemsViewElement::OnPropertyChange(const Rml::PropertyIdSet& changed){
    Rml::Element::OnPropertyChange(changed);
    if (changed.Contains(Rml::PropertyId::Width) || changed.Contains(Rml::PropertyId::Height) ||
        changed.Contains(Rml::PropertyId::Display) || changed.Contains(Rml::PropertyId::Position) ||
        changed.Contains(Rml::PropertyId::OverflowY) || changed.Contains(Rml::PropertyId::MaxHeight)){
        heightChecked = false;
        updateBoundary();
    }
}

void ItemsViewElement::OnResize(){
    Rml::Element::OnResize();
    // the viewport changed in the layout: the window follows at the next update
    windowDirty = true;
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0);
}

// ---------------------------------------------------------------- data sources

bool ItemsViewElement::bindModel(){
    if (model || providerSet || itemsExpression.empty())
        return model != nullptr;
    Rml::DataModel* dataModel = GetDataModel();
    if (!dataModel)
        return false;
    Rml::DataAddress address = dataModel->ResolveAddress(itemsExpression, this);
    if (address.empty()){
        Log::warn("[HTML UI] <%s items=\"%s\">: no such data model variable", GetTagName().c_str(), itemsExpression.c_str());
        itemsExpression.clear();
        return false;
    }
    model = dataModel;
    containerAddress = std::move(address);
    modelName.clear();
    for (Rml::Element* each = this; each; each = each->GetParentNode()){
        const Rml::String name = each->GetAttribute<Rml::String>("data-model", "");
        if (!name.empty()){
            modelName = name;
            break;
        }
    }
    model->AddView(Rml::DataViewPtr(new ItemsSourceView(this, containerAddress.front().name)));
    modelDirty = true;
    // rows made for another source go: model rows have aliases
    while (!pool.empty())
        DestroyTailSlot();
    core.Reset();
    return true;
}

void ItemsViewElement::unbindModel(){
    if (!model)
        return;
    while (!pool.empty())
        DestroyTailSlot();
    model = nullptr;
    containerAddress.clear();
    modelName.clear();
    itemCount = 0;
    core.Reset();
    windowDirty = true;
}

void ItemsViewElement::onModelChanged(){
    modelDirty = true;
    windowDirty = true;
    if (!structureReady || !model)
        return;
    // in the data model's update, before the views of the rows (they are deeper): the rows show
    // items that exist when their views evaluate (an array that shrank leaves none past its end)
    inModelUpdate = true;
    updateWindow(VirtualizationCoordinator::Reason::DataChanged, false);
    inModelUpdate = false;
}

void ItemsViewElement::readModelCount(){
    modelDirty = false;
    if (!model)
        return;
    if (kind == Kind::Tree){
        treeDirty = true;
        rebuildTree();
        rebindAll = true;
        return;
    }
    Rml::DataVariable variable = model->GetVariable(containerAddress);
    const int count = variable ? std::max(0, variable.Size()) : 0;
    if (count != itemCount){
        if (anchorEnd && count > itemCount && atEnd)
            pendingScrollIndex = std::max(0, count - 1), pendingAlign = Align::End;
        itemCount = count;
        heightChecked = false;
    }
    // the bound rows evaluate their views again with the model; a rebind keeps rows that show the same item
    rebindAll = true;
}

Rml::DataAddress ItemsViewElement::itemAddress(int index) const{
    if (kind == Kind::Tree)
        return treeAddress(index);
    Rml::DataAddress address = containerAddress;
    address.push_back(Rml::DataAddressEntry(index));
    return address;
}

void ItemsViewElement::setProvider(Provider value){
    ensureStructure();
    if (model)
        unbindModel();
    while (!pool.empty())
        DestroyTailSlot();
    core.Reset();
    provider = std::move(value);
    providerSet = true;
    childrenCache.clear();
    hasChildrenCache.clear();
    selectedKeys.clear();
    active = -1;
    anchor = -1;
    if (kind == Kind::Tree){
        treeDirty = true;
        rebuildTree();
    }else{
        itemCount = std::max(0, provider.count);
    }
    heights.reset(0, heights.estimate());
    rebindAll = true;
    windowDirty = true;
    updateWindow(VirtualizationCoordinator::Reason::DataChanged, false);
}

void ItemsViewElement::clearProvider(){
    if (!providerSet)
        return;
    while (!pool.empty())
        DestroyTailSlot();
    provider = Provider();
    providerSet = false;
    childrenCache.clear();
    hasChildrenCache.clear();
    itemCount = 0;
    nodes.clear();
    core.Reset();
    windowDirty = true;
}

void ItemsViewElement::setCount(int count){
    ensureStructure();
    if (kind == Kind::Tree){
        refresh();
        return;
    }
    count = std::max(0, count);
    if (providerSet)
        provider.count = count;
    if (count == itemCount)
        return;
    if (anchorEnd && count > itemCount && atEnd)
        pendingScrollIndex = count - 1, pendingAlign = Align::End;
    itemCount = count;
    heightChecked = false;
    windowDirty = true;
    updateWindow(VirtualizationCoordinator::Reason::DataChanged, false);
}

void ItemsViewElement::refresh(){
    if (kind == Kind::Tree){
        // a provider's structure is asked again
        childrenCache.clear();
        hasChildrenCache.clear();
        treeDirty = true;
        rebuildTree();
    }
    rebindAll = true;
    windowDirty = true;
    if (providerSet || model)
        updateWindow(VirtualizationCoordinator::Reason::DataChanged, false);
}

void ItemsViewElement::refreshItems(const std::vector<int>& indices){
    for (int index : indices){
        for (size_t slot = 0; slot < pool.size(); slot++){
            if (pool[slot].item == index)
                refreshSlots.push_back((int)slot);
        }
    }
    if (refreshSlots.empty())
        return;
    windowDirty = true;
    updateWindow(VirtualizationCoordinator::Reason::DataChanged, false);
}

std::string ItemsViewElement::keyOf(int index) const{
    if (index < 0)
        return std::string();
    if (kind == Kind::Tree)
        return index < (int)nodes.size() ? nodes[index].key : std::string();
    if (providerSet){
        if (provider.key)
            return provider.key(index);
        return std::to_string(index);
    }
    if (model && !keyField.empty()){
        Rml::DataAddress address = itemAddress(index);
        address.emplace_back(keyField);
        Rml::Variant value;
        if (Rml::DataVariable variable = model->GetVariable(address)){
            if (variable.Get(value))
                return variantText(value);
        }
    }
    return std::to_string(index);
}

int ItemsViewElement::indexOfKey(const std::string& key) const{
    if (kind == Kind::Tree){
        for (int i = 0; i < (int)nodes.size(); i++){
            if (nodes[i].key == key)
                return i;
        }
        return -1;
    }
    const bool keyed = (providerSet && provider.key) || (model && !keyField.empty());
    if (!keyed){
        char* end = nullptr;
        const long index = std::strtol(key.c_str(), &end, 10);
        if (end == key.c_str() || *end != '\0' || index < 0 || index >= itemCount)
            return -1;
        return (int)index;
    }
    for (int i = 0; i < itemCount; i++){
        if (keyOf(i) == key)
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------- rows (VirtualWindowCore::Host)

void ItemsViewElement::EnsurePool(int slotCount){
    while ((int)pool.size() < slotCount){
        Row row;
        row.element = makeRowElement();
        pool.push_back(std::move(row));
    }
}

int ItemsViewElement::SlotCount() const{
    return (int)pool.size();
}

void ItemsViewElement::clearRowContent(Row& row){
    if (!row.element)
        return;
    while (row.element->GetNumChildren() > 0)
        row.element->RemoveChild(row.element->GetChild(0));
    row.contentMade = false;
}

// the template's content into a row that has none yet (false while the template is not readable)
bool ItemsViewElement::makeRowContent(Row& row){
    if (row.contentMade)
        return true;
    const std::string rml = rowContentRml();
    if (rml.empty()){
        // the template is not readable yet (an HTML document's compile is bound once it loaded)
        contentPending = true;
        return false;
    }
    row.element->SetInnerRML(rml);
    row.contentMade = true;
    createdCounter()++;
    return true;
}

void ItemsViewElement::bindRowContent(Row& row, int item){
    Rml::Element* element = row.element;
    const bool sameItem = (row.item == item);

    if (providerSet){
        makeRowContent(row);
        if (provider.unbind && row.item >= 0)
            provider.unbind(element, row.item);
        if (provider.bind)
            provider.bind(element, item);
        return;
    }
    if (!model)
        return;

    // the model's aliases: item -> items[k] (tree: the node's path), i -> k (tree: depth, expanded, hasChildren too)
    std::vector<Rml::DataAddress> targets;
    targets.push_back(itemAddress(item));
    Rml::DataAddress index;
    index.emplace_back("literal");
    index.emplace_back("int");
    index.emplace_back(item);
    targets.push_back(index);
    std::vector<std::string> names{aliasItem, aliasIndex};
    if (kind == Kind::Tree){
        const TreeNodeInfo node = nodeAt(item);
        for (int value : {node.depth, node.expanded ? 1 : 0, node.hasChildren ? 1 : 0}){
            Rml::DataAddress literal;
            literal.emplace_back("literal");
            literal.emplace_back("int");
            literal.emplace_back(value);
            targets.push_back(literal);
        }
        names.push_back("depth");
        names.push_back("expanded");
        names.push_back("hasChildren");
    }

    if (useRowAliases()){
        // P7-D: the row's aliases are re-pointed, its content stays
        if (row.aliases.empty()){
            for (size_t i = 0; i < targets.size(); i++)
                row.aliases.push_back(model->InsertRowAlias(element, names[i], targets[i]));
        }else{
            for (size_t i = 0; i < targets.size() && i < row.aliases.size(); i++)
                model->SetRowAlias(row.aliases[i], targets[i]);
        }
        makeRowContent(row);
        return;
    }

    // without P7-D the row's content is made again for another item (its views resolved their
    // addresses when they were made); the row element keeps its identity
    if (row.contentMade && sameItem && kind != Kind::Tree)
        return;
    clearRowContent(row);
    model->EraseAliases(element);
    for (size_t i = 0; i < targets.size(); i++)
        model->InsertAlias(element, names[i], targets[i]);
    makeRowContent(row);
}

void ItemsViewElement::setRowClasses(Row& row){
    Rml::Element* element = row.element;
    const int item = row.item;
    element->SetClass("odd", (item % 2) == 1);
    element->SetClass("even", (item % 2) == 0);
    const bool selected = selectedKeys.count(row.key) > 0;
    element->SetClass("selected", selected);
    element->SetAttribute("aria-selected", selected ? "true" : "false");
    element->SetAttribute("aria-rowindex", item + 1);
    if (kind == Kind::Tree){
        const TreeNodeInfo node = nodeAt(item);
        element->SetAttribute("aria-level", node.depth + 1);
        element->SetClass("expanded", node.expanded);
        element->SetClass("has-children", node.hasChildren);
        if (node.hasChildren)
            element->SetAttribute("aria-expanded", node.expanded ? "true" : "false");
        else
            element->RemoveAttribute("aria-expanded");
        element->SetProperty(Rml::PropertyId::PaddingLeft, px((float)node.depth * indent));
    }
}

void ItemsViewElement::positionRow(Row& row, int slot){
    (void)slot;
    Rml::Element* element = row.element;
    const int line = row.item / std::max(1, lanes);
    element->SetProperty(Rml::PropertyId::Top, px(heights.offsetOf(line)));
    if (kind == Kind::Grid){
        const int lane = row.item % std::max(1, lanes);
        element->SetProperty(Rml::PropertyId::Left, px((float)lane * (itemWidth + gap)));
        element->SetProperty(Rml::PropertyId::Width, px(itemWidth));
        element->SetProperty(Rml::PropertyId::Height, px(itemHeight));
    }else if (!autoHeight){
        element->SetProperty(Rml::PropertyId::Height, px(itemHeight));
    }
    if (row.hidden){
        element->RemoveProperty(Rml::PropertyId::Display);
        row.hidden = false;
    }
}

VirtualWindowCore::Impact ItemsViewElement::Rebind(int slot, int itemIndex){
    const std::string key = keyOf(itemIndex);

    // identity (R6): the element that shows this key keeps showing it, when its slot has not
    // been bound by this pass yet
    auto found = keySlots.find(key);
    if (found != keySlots.end() && found->second != slot && found->second < (int)pool.size() && pool[found->second].pass != pass){
        const int other = found->second;
        std::swap(pool[slot], pool[other]);
        if (pool[other].item >= 0)
            keySlots[pool[other].key] = other;
        keySlots[key] = slot;
    }

    Row& row = pool[slot];
    const bool sameBinding = (row.item == itemIndex && row.key == key);
    VirtualWindowCore::Impact impact = VirtualWindowCore::Impact::None;
    if (!sameBinding || rebindAll || (providerSet && std::find(refreshSlots.begin(), refreshSlots.end(), slot) != refreshSlots.end())){
        if (row.item >= 0 && row.key != key){
            auto mine = keySlots.find(row.key);
            if (mine != keySlots.end() && mine->second == slot)
                keySlots.erase(mine);
        }
        bindRowContent(row, itemIndex);
        impact = VirtualWindowCore::Impact::Rebind;
    }
    if (impact != VirtualWindowCore::Impact::None)
        row.laidOutAfter = inUpdate ? updates : updates + 1;
    row.item = itemIndex;
    row.key = key;
    row.pass = pass;
    keySlots[key] = slot;
    setRowClasses(row);
    positionRow(row, slot);
    if (autoHeight && !heights.isMeasured(itemIndex)){
        // its height arrives with the next layout (measureRows)
        row.element->RemoveProperty(Rml::PropertyId::Height);
    }

    // a focused row recycled away comes back focused (R8)
    if (focusRowWanted && itemIndex == active){
        focusRowWanted = false;
        row.element->Focus();
    }
    return impact;
}

void ItemsViewElement::UnbindSlot(int slot){
    if (slot < 0 || slot >= (int)pool.size())
        return;
    Row& row = pool[slot];
    bool hadFocus = false;
    if (row.item >= 0){
        if (Rml::Context* context = GetContext()){
            for (Rml::Element* each = context->GetFocusElement(); each && each != this; each = each->GetParentNode())
                hadFocus |= (each == row.element);
        }
        if (providerSet && provider.unbind)
            provider.unbind(row.element, row.item);
        auto mine = keySlots.find(row.key);
        if (mine != keySlots.end() && mine->second == slot)
            keySlots.erase(mine);
    }
    // a hidden row's views still evaluate when the model changes: they read an item that exists
    if (model && row.contentMade){
        if (!row.aliases.empty() && itemCount > 0 && (kind != Kind::Tree || !nodes.empty()))
            model->SetRowAlias(row.aliases[0], itemAddress(0));
        else
            clearRowContent(row);
    }
    row.item = -1;
    row.key.clear();
    row.pass = pass;
    if (!row.hidden){
        row.element->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::None));
        row.hidden = true;
    }
    // the focus leaves with the row: to the view, back to the row when it shows again (R8)
    if (hadFocus){
        focusRowWanted = true;
        Focus();
    }
}

bool ItemsViewElement::SlotBinds(int slot, int itemIndex) const{
    return slot >= 0 && slot < (int)pool.size() && pool[slot].item == itemIndex;
}

void ItemsViewElement::DestroyTailSlot(){
    if (pool.empty())
        return;
    Row& row = pool.back();
    if (row.item >= 0){
        if (providerSet && provider.unbind)
            provider.unbind(row.element, row.item);
        auto mine = keySlots.find(row.key);
        if (mine != keySlots.end() && mine->second == (int)pool.size() - 1)
            keySlots.erase(mine);
    }
    if (row.element && row.element->GetParentNode())
        row.element->GetParentNode()->RemoveChild(row.element);
    pool.pop_back();
}

// ---------------------------------------------------------------- window

int ItemsViewElement::lineCount() const{
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    const int perLine = std::max(1, lanes);
    return (count + perLine - 1) / perLine;
}

float ItemsViewElement::rowsTop() const{
    return sizer ? const_cast<Rml::Element*>(sizer)->GetOffsetTop() : 0.0f;
}

float ItemsViewElement::sizerScroll() const{
    auto* self = const_cast<ItemsViewElement*>(this);
    if (header)
        return std::max(0.0f, self->GetScrollTop());
    return std::max(0.0f, self->GetScrollTop() - rowsTop());
}

float ItemsViewElement::scrollForSizer(float y) const{
    return header ? y : y + rowsTop();
}

float ItemsViewElement::viewportHeight() const{
    auto* self = const_cast<ItemsViewElement*>(this);
    if (indefiniteHeight)
        return (float)kIndefiniteRows * heights.estimate();
    float height = self->GetClientHeight();
    if (header)
        height -= header->GetBox().GetSize(Rml::BoxArea::Border).y;
    if (height > 0.0f)
        return height;
    // not laid out yet: a definite height, else the rows of a view without one (R13)
    const Rml::Style::Height computed = self->GetComputedValues().height();
    if (computed.type == Rml::Style::Height::Length && computed.value > 0.0f)
        return computed.value;
    return (float)kIndefiniteRows * heights.estimate();
}

void ItemsViewElement::updateSizer(){
    if (!sizer)
        return;
    const float total = std::max(0.0f, heights.total() - (kind == Kind::Grid && lineCount() > 0 ? gap : 0.0f));
    sizer->SetProperty(Rml::PropertyId::Height, px(total));
    if (kind == Kind::Table){
        float width = 0.0f;
        for (const Column& column : columns)
            width += column.width;
        sizer->SetProperty(Rml::PropertyId::Width, px(width));
    }
}

void ItemsViewElement::measureRows(){
    if (!autoHeight || measuring)
        return;
    measuring = true;
    // scroll anchoring (R3): the first visible row keeps its place on screen
    const float scroll = sizerScroll();
    const int firstVisible = heights.indexAt(scroll);
    const float anchorDelta = firstVisible >= 0 ? heights.offsetOf(firstVisible) - scroll : 0.0f;

    bool changed = false;
    for (Row& row : pool){
        if (row.item < 0 || row.hidden || row.laidOutAfter >= updates)
            continue;
        const float height = row.element->GetBox().GetSize(Rml::BoxArea::Border).y;
        if (height <= 0.0f && row.element->GetNumChildren() > 0 && row.element->GetBox().GetSize().x <= 0.0f)
            continue;   // not laid out yet
        if (heights.measuredCount() == 0 && estimatedHeight <= 0.0f && height > 0.0f)
            heights.setEstimate(height);
        if (!heights.isMeasured(row.item) || std::abs(heights.heightOf(row.item) - height) > 0.01f){
            heights.set(row.item, height);
            changed = true;
        }
        if (height > 0.0f)
            minLineHeight = std::min(minLineHeight > 0.0f ? minLineHeight : height, height);
    }
    if (changed){
        for (int slot = 0; slot < (int)pool.size(); slot++){
            if (pool[slot].item >= 0 && !pool[slot].hidden)
                positionRow(pool[slot], slot);
        }
        updateSizer();
        if (firstVisible >= 0 && scroll > 0.0f){
            const float target = heights.offsetOf(firstVisible) - anchorDelta;
            if (std::abs(target - scroll) >= 0.5f)
                SetScrollTop(scrollForSizer(target));
        }
        windowDirty = true;
    }
    measuring = false;
}

void ItemsViewElement::updateWindow(VirtualizationCoordinator::Reason reason, bool allowShrink){
    ensureStructure();
    if (!sizer)
        return;
    if (model && modelDirty)
        readModelCount();
    if (kind == Kind::Tree && treeDirty)
        rebuildTree();

    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    if (kind == Kind::Grid){
        const float width = GetClientWidth();
        const int newLanes = std::max(1, (int)std::floor((width + gap) / (itemWidth + gap)));
        lanes = width > 0.0f ? newLanes : 1;
    }else{
        lanes = 1;
    }
    const int lines = lineCount();
    if (heights.count() != lines){
        if (!autoHeight && !providerSet){
            heights.reset(lines, heights.estimate());
        }else{
            heights.resize(lines);
        }
        if (providerSet && provider.height && kind != Kind::Grid){
            for (int i = 0; i < lines; i++){
                if (!heights.isMeasured(i))
                    heights.set(i, provider.height(i));
            }
        }
    }

    const float viewport = std::max(1.0f, viewportHeight());
    const float scroll = sizerScroll();
    const float lineHeight = std::max(kMinLineHeight, std::min(heights.estimate(), minLineHeight > 0.0f ? minLineHeight : heights.estimate()));
    int desired = 0;
    int first = 0;
    if (lines > 0){
        const int firstVisible = std::max(0, heights.indexAt(scroll));
        const int lastVisible = std::max(firstVisible, heights.indexAt(scroll + viewport - 0.5f));
        desired = std::max((int)std::ceil(viewport / lineHeight) + 1, lastVisible - firstVisible + 1) + 2 * overscan;
        // a window a few lines smaller keeps its size (variable heights vary the visible lines):
        // its ring keeps shifting instead of binding every row again
        const VirtualWindowCore::Guard& last = core.LastGuard();
        if (last.Desired > desired && last.Desired - desired <= VirtualWindowCore::kShrinkSlackLines &&
            last.ViewportWPx == (int)std::lround(GetClientWidth()))
            desired = last.Desired;
        first = std::max(0, firstVisible - overscan);
        if (first + desired > lines)
            first = std::max(0, lines - desired);
    }

    VirtualWindowCore::Guard guard;
    guard.First = first;
    guard.Desired = lines > 0 ? desired : 0;
    guard.ItemCount = count;
    guard.ContentHPx = 0;
    guard.ViewportWPx = (int)std::lround(GetClientWidth());
    guard.ContentWPx = lanes;
    guard.StructureGeneration = 1;

    VirtualWindowCore::ChangeSetView changes;
    if (rebindAll){
        changes.ChangeKind = VirtualWindowCore::ChangeSetView::Kind::All;
    }else if (!refreshSlots.empty()){
        changes.ChangeKind = VirtualWindowCore::ChangeSetView::Kind::Subset;
        changes.AffectedSlots = std::span<const int>(refreshSlots.data(), refreshSlots.size());
    }

    pass++;
    core.Update(guard, reason, lanes, changes, *this, allowShrink);
    rebindAll = false;
    refreshSlots.clear();
    windowDirty = false;

    updateSizer();
    // the views of rows bound now evaluate in this frame, not the next (the data models updated
    // before the elements); during the model's own update they evaluate after this view
    if (model && !inModelUpdate)
        model->Update(false);
}

void ItemsViewElement::OnUpdate(){
    Rml::Element::OnUpdate();
    ensureStructure();

    if (focusRowFromView){
        focusRowFromView = false;
        Rml::Context* context = GetContext();
        Rml::Element* row = rowOf(active);
        if (context && row && context->GetFocusElement() == this){
            focusRowWanted = false;
            row->Focus(true);
        }
    }

    if (model && GetDataModel() != model)
        unbindModel();
    if (!model && !providerSet && !itemsExpression.empty())
        bindModel();

    if (!model && !providerSet){
        return;
    }

    updates++;
    inUpdate = true;
    if (contentPending && !rowContentRml().empty()){
        contentPending = false;
        rebindAll = true;
        windowDirty = true;
    }
    checkHeight();
    measureRows();

    // a scroll asked for before the view was laid out, or stick-to-end (R12)
    // R12: a view stuck to its end stays there while its rows grow (appended, or measured taller)
    const float maxScrollNow = GetScrollHeight() - GetClientHeight();
    if (anchorEnd && atEnd && pendingScrollIndex < 0 && maxScrollNow > lastMaxScroll + 0.5f && GetScrollTop() < maxScrollNow - 1.0f)
        SetScrollTop(maxScrollNow);
    lastMaxScroll = maxScrollNow;

    // (after the sizer took its new height: a scroll to rows past the old end waits for the layout)
    const bool sizerLaidOut = !sizer || sizer->GetBox().GetSize().y + 0.5f >= heights.total() - (kind == Kind::Grid ? gap : 0.0f);
    if (pendingScrollIndex >= 0 && (GetScrollHeight() > 0.0f || itemCount == 0) && (sizerLaidOut || pendingWaits >= 3)){
        const int index = pendingScrollIndex;
        pendingScrollIndex = -1;
        pendingWaits = 0;
        scrollToIndex(index, pendingAlign);
    }else if (pendingScrollIndex >= 0){
        pendingWaits++;
    }

    const float scrollTop = GetScrollTop();
    if (scrollTop != lastScrollTop){
        lastScrollTop = scrollTop;
        windowDirty = true;
    }
    // a drag from one of the rows keeps the pool whole until it ends (R22)
    const bool allowShrink = !DragDropSession::isDragSourceIn(this);
    if (windowDirty || modelDirty)
        updateWindow(modelDirty ? VirtualizationCoordinator::Reason::DataChanged : VirtualizationCoordinator::Reason::ScrollChanged, allowShrink);
    else
        core.Update(core.LastGuard(), VirtualizationCoordinator::Reason::Prewarm, lanes, VirtualWindowCore::ChangeSetView{}, *this, allowShrink);

    const float maxScroll = GetScrollHeight() - GetClientHeight();
    atEnd = GetScrollTop() >= maxScroll - 1.0f;
    inUpdate = false;
}

// ---------------------------------------------------------------- definite size, layout boundary

bool ItemsViewElement::hasDefiniteSize() const{
    auto* self = const_cast<ItemsViewElement*>(this);
    const Rml::ComputedValues& computed = self->GetComputedValues();
    using namespace Rml::Style;
    const Display display = computed.display();
    if (display == Display::None || display == Display::Inline || display == Display::Contents)
        return false;
    if (computed.position() == Position::Static)
        return false;
    if (computed.overflow_y() != Overflow::Auto && computed.overflow_y() != Overflow::Scroll && computed.overflow_y() != Overflow::Hidden)
        return false;
    if (indefiniteHeight)
        return true;   // the height the view gave itself
    if (computed.height().type == Height::Auto)
        return false;
    // an automatic width is its container's only in a block container's flow
    if (computed.width().type == Width::Auto){
        if (computed.float_() != Float::None || computed.position() == Position::Absolute || computed.position() == Position::Fixed ||
            display == Display::InlineBlock || display == Display::InlineFlex || display == Display::InlineGrid)
            return false;
        Rml::Element* parent = self->GetParentNode();
        if (!parent)
            return false;
        const Display parentDisplay = parent->GetComputedValues().display();
        if (parentDisplay == Display::Flex || parentDisplay == Display::InlineFlex || parentDisplay == Display::Grid ||
            parentDisplay == Display::InlineGrid)
            return false;
    }
    return true;
}

void ItemsViewElement::updateBoundary(){
    const bool wanted = useLayoutBoundary() && hasDefiniteSize();
    if (wanted != boundary){
        boundary = wanted;
        SetLayoutBoundary(wanted);
    }
}

void ItemsViewElement::checkHeight(){
    if (heightChecked)
        return;
    const float client = GetClientHeight();
    if (client <= 0.0f)
        return;
    heightChecked = true;
    const Rml::ComputedValues& computed = GetComputedValues();
    const float total = heights.total();
    const bool grewToContent = computed.height().type == Rml::Style::Height::Auto && computed.max_height().type == Rml::Style::MaxHeight::Length &&
                               computed.max_height().value >= FLT_MAX * 0.5f &&
                               lineCount() > kIndefiniteRows && client + 1.0f >= total;
    if (grewToContent && !indefiniteHeight){
        // R13: HTMLUI-HTML-0029; the view lays out 20 rows instead of every row
        indefiniteHeight = true;
        SetProperty(Rml::PropertyId::Height, px((float)kIndefiniteRows * heights.estimate()));
        WidgetsRuntime::reportElement(this, WidgetsRuntime::Code::VirtualViewHeight,
                                      "<" + GetTagName() + "> has no definite height: it lays out " + std::to_string(kIndefiniteRows) + " rows",
                                      "give it a height (height, max-height, or flex: 1 in a container of a definite height)");
        windowDirty = true;
    }
    updateBoundary();
}

// ---------------------------------------------------------------- scrolling

void ItemsViewElement::scrollToIndex(int index, Align align){
    ensureStructure();
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    if (count <= 0 || index < 0)
        return;
    index = std::min(index, count - 1);
    if (GetScrollHeight() <= 0.0f || GetClientHeight() <= 0.0f){
        // not laid out yet: once it is
        pendingScrollIndex = index;
        pendingAlign = align;
        return;
    }
    const int line = index / std::max(1, lanes);
    const float top = heights.offsetOf(line);
    const float bottom = top + heights.heightOf(line);
    const float viewport = viewportHeight();
    const float scroll = sizerScroll();
    float target = scroll;
    switch (align){
        case Align::Start: target = top; break;
        case Align::End: target = bottom - viewport; break;
        case Align::Center: target = (top + bottom - viewport) * 0.5f; break;
        case Align::Nearest:
            if (top < scroll)
                target = top;
            else if (bottom > scroll + viewport)
                target = bottom - viewport;
            break;
    }
    target = std::max(0.0f, target);
    // the sizer is already as tall as every row: the scroll range covers the target
    SetScrollTop(scrollForSizer(target));
    lastScrollTop = GetScrollTop();
    windowDirty = true;
    updateWindow(VirtualizationCoordinator::Reason::ScrollChanged, false);
}

std::pair<int, int> ItemsViewElement::getVisibleRange() const{
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    if (count <= 0 || heights.count() == 0)
        return {-1, -1};
    const float scroll = sizerScroll();
    const float viewport = viewportHeight();
    const int perLine = std::max(1, lanes);
    const int firstLine = std::max(0, heights.indexAt(scroll));
    const int lastLine = std::max(firstLine, heights.indexAt(scroll + viewport - 0.5f));
    return {std::min(count - 1, firstLine * perLine), std::min(count - 1, lastLine * perLine + perLine - 1)};
}

// ---------------------------------------------------------------- rows from elements, drops

int ItemsViewElement::itemOfElement(Rml::Element* element) const{
    for (Rml::Element* each = element; each && each != this; each = each->GetParentNode()){
        if (isRowElement(each) && each->GetParentNode() == sizer){
            for (const Row& row : pool){
                if (row.element == each)
                    return row.item;
            }
            return -1;
        }
    }
    return -1;
}

Rml::Element* ItemsViewElement::rowOf(int index) const{
    if (index < 0)
        return nullptr;
    for (const Row& row : pool){
        if (row.item == index && !row.hidden)
            return row.element;
    }
    return nullptr;
}

bool ItemsViewElement::dropTargetAt(float x, float y, int& index, int& band) const{
    index = -1;
    band = 0;
    if (!sizer)
        return false;
    auto* self = const_cast<ItemsViewElement*>(this);
    if (!self->IsPointWithinElement(Rml::Vector2f(x, y)))
        return false;
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    const Rml::Vector2f origin = const_cast<Rml::Element*>(sizer)->GetAbsoluteOffset(Rml::BoxArea::Border);
    const float localY = y - origin.y;
    if (count <= 0){
        index = 0;
        band = -1;
        return true;
    }
    const int line = heights.indexAt(localY);
    const int perLine = std::max(1, lanes);
    int item = line * perLine;
    if (kind == Kind::Grid)
        item += std::clamp((int)std::floor((x - origin.x) / (itemWidth + gap)), 0, perLine - 1);
    item = std::clamp(item, 0, count - 1);
    const float top = heights.offsetOf(line);
    const float height = std::max(1.0f, heights.heightOf(line));
    const float within = (localY - top) / height;
    index = item;
    if (localY >= heights.total())
        band = 1;
    else
        band = within < 0.25f ? -1 : (within > 0.75f ? 1 : 0);
    return true;
}

void ItemsViewElement::showDropIndicator(int index, int band){
    if (!indicator)
        return;
    if (index < 0){
        indicator->SetProperty(Rml::PropertyId::Display, Rml::Property(Rml::Style::Display::None));
        return;
    }
    const int line = index / std::max(1, lanes);
    const float top = heights.offsetOf(line);
    const float height = heights.heightOf(line);
    indicator->RemoveProperty(Rml::PropertyId::Display);
    indicator->SetClass("htmlui-drop-on", band == 0);
    indicator->SetClass("htmlui-drop-before", band < 0);
    indicator->SetClass("htmlui-drop-after", band > 0);
    if (band == 0){
        indicator->SetProperty(Rml::PropertyId::Top, px(top));
        indicator->SetProperty(Rml::PropertyId::Height, px(height));
    }else{
        indicator->SetProperty(Rml::PropertyId::Top, px(band < 0 ? top : top + height));
        indicator->RemoveProperty(Rml::PropertyId::Height);
    }
    if (kind == Kind::Grid){
        const int lane = index % std::max(1, lanes);
        indicator->SetProperty(Rml::PropertyId::Left, px((float)lane * (itemWidth + gap)));
        indicator->SetProperty(Rml::PropertyId::Width, px(itemWidth));
    }
}

// ---------------------------------------------------------------- selection

bool ItemsViewElement::isSelected(int index) const{
    return index >= 0 && selectedKeys.count(keyOf(index)) > 0;
}

std::vector<int> ItemsViewElement::getSelection() const{
    std::vector<int> indices;
    if (selectedKeys.empty())
        return indices;
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    const bool keyed = kind == Kind::Tree || (providerSet && provider.key) || (model && !keyField.empty());
    if (!keyed){
        for (const std::string& key : selectedKeys){
            const int index = indexOfKey(key);
            if (index >= 0)
                indices.push_back(index);
        }
    }else{
        for (int i = 0; i < count && indices.size() < selectedKeys.size(); i++){
            if (selectedKeys.count(keyOf(i)))
                indices.push_back(i);
        }
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

std::vector<std::string> ItemsViewElement::getSelectedKeys() const{
    std::vector<std::string> keys;
    for (int index : getSelection())
        keys.push_back(keyOf(index));
    return keys;
}

void ItemsViewElement::setSelection(const std::vector<int>& indices, bool notify){
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    selectedKeys.clear();
    if (selectionMode != SelectionMode::None){
        for (int index : indices){
            if (index < 0 || index >= count)
                continue;
            selectedKeys.insert(keyOf(index));
            if (selectionMode == SelectionMode::Single)
                break;
        }
    }
    for (Row& row : pool){
        if (row.item >= 0)
            setRowClasses(row);
    }
    if (notify)
        notifySelection();
}

void ItemsViewElement::selectRange(int from, int to, bool add){
    if (!add)
        selectedKeys.clear();
    if (from > to)
        std::swap(from, to);
    for (int i = std::max(0, from); i <= to; i++)
        selectedKeys.insert(keyOf(i));
}

void ItemsViewElement::selectFromPointer(int index, bool toggleKey, bool extend){
    if (selectionMode == SelectionMode::None || index < 0)
        return;
    const std::string key = keyOf(index);
    if (selectionMode == SelectionMode::Single){
        selectedKeys.clear();
        selectedKeys.insert(key);
        anchor = index;
    }else if (extend && anchor >= 0){
        selectRange(anchor, index, toggleKey);
    }else if (toggleKey){
        if (!selectedKeys.erase(key))
            selectedKeys.insert(key);
        anchor = index;
    }else{
        selectedKeys.clear();
        selectedKeys.insert(key);
        anchor = index;
    }
    for (Row& row : pool){
        if (row.item >= 0)
            setRowClasses(row);
    }
    notifySelection();
}

void ItemsViewElement::notifySelection(){
    UIValue detail = UIValue::object();
    UIValue indices = UIValue::array();
    UIValue keys = UIValue::array();
    for (int index : getSelection()){
        indices.push(UIValue(index + 1));
        keys.push(UIValue(keyOf(index)));
    }
    detail["indices"] = indices;
    detail["keys"] = keys;
    Rml::Dictionary parameters;
    parameters[HtmlUIEventNames::detailParameter] = (void*)&detail;
    DispatchEvent("select", parameters, false, true);
}

void ItemsViewElement::dispatchDetail(const char* type, int index){
    UIValue detail = UIValue::object();
    detail["index"] = UIValue(index + 1);
    detail["key"] = UIValue(keyOf(index));
    Rml::Dictionary parameters;
    parameters[HtmlUIEventNames::detailParameter] = (void*)&detail;
    DispatchEvent(type, parameters, false, true);
}

void ItemsViewElement::activate(int index){
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    if (index < 0 || index >= count)
        return;
    dispatchDetail("activate", index);
}

void ItemsViewElement::setActiveIndex(int index){
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    if (count <= 0)
        return;
    active = std::clamp(index, 0, count - 1);
    scrollToIndex(active, Align::Nearest);
    focusActiveRow();
}

void ItemsViewElement::focusActiveRow(){
    Rml::Context* context = GetContext();
    if (!context)
        return;
    bool focusedInside = false;
    for (Rml::Element* each = context->GetFocusElement(); each; each = each->GetParentNode()){
        if (each == this){
            focusedInside = true;
            break;
        }
    }
    if (!focusedInside)
        return;
    if (Rml::Element* row = rowOf(active)){
        focusRowWanted = false;
        row->Focus(true);
    }else{
        focusRowWanted = true;
        Focus(true);
    }
}

void ItemsViewElement::moveActive(int index, bool extend){
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    if (count <= 0)
        return;
    index = std::clamp(index, 0, count - 1);
    if (selectionMode != SelectionMode::None){
        if (extend && selectionMode == SelectionMode::Multiple){
            if (anchor < 0)
                anchor = active >= 0 ? active : index;
            selectRange(anchor, index, false);
        }else{
            selectedKeys.clear();
            selectedKeys.insert(keyOf(index));
            anchor = index;
        }
    }
    active = index;
    scrollToIndex(index, Align::Nearest);
    for (Row& row : pool){
        if (row.item >= 0)
            setRowClasses(row);
    }
    focusActiveRow();
    if (selectionMode != SelectionMode::None)
        notifySelection();
}

bool ItemsViewElement::handleKey(Rml::Event& event){
    const int count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    if (count <= 0)
        return false;
    const int key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
    const bool shift = event.GetParameter<int>("shift_key", 0) != 0;
    const bool ctrl = event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0;
    const int perLine = std::max(1, lanes);
    const int current = active >= 0 ? std::min(active, count - 1) : -1;

    switch (key){
        case Rml::Input::KI_UP:
            if (current < 0){ moveActive(0, false); return true; }
            if (current - perLine < 0) return false;   // at the edge: spatial navigation leaves the view (R8)
            moveActive(current - perLine, shift);
            return true;
        case Rml::Input::KI_DOWN:
            if (current < 0){ moveActive(0, false); return true; }
            if (current + perLine >= count) return false;
            moveActive(current + perLine, shift);
            return true;
        case Rml::Input::KI_LEFT:
            if (kind == Kind::Grid){
                if (current <= 0 || current % perLine == 0) return false;
                moveActive(current - 1, shift);
                return true;
            }
            if (kind == Kind::Tree && current >= 0){
                const TreeNodeInfo node = nodeAt(current);
                if (node.hasChildren && node.expanded){
                    collapse(node.key);
                    return true;
                }
                if (node.parent >= 0){
                    moveActive(node.parent, false);
                    return true;
                }
            }
            return false;
        case Rml::Input::KI_RIGHT:
            if (kind == Kind::Grid){
                if (current < 0 || current + 1 >= count || (current + 1) % perLine == 0) return false;
                moveActive(current + 1, shift);
                return true;
            }
            if (kind == Kind::Tree && current >= 0){
                const TreeNodeInfo node = nodeAt(current);
                if (node.hasChildren && !node.expanded){
                    expand(node.key);
                    return true;
                }
                if (node.hasChildren && current + 1 < count){
                    moveActive(current + 1, false);
                    return true;
                }
            }
            return false;
        case Rml::Input::KI_PRIOR:
        case Rml::Input::KI_NEXT:{
            // one viewport (R8)
            const int line = std::max(0, current) / perLine;
            const float viewport = viewportHeight();
            const float y = heights.offsetOf(line) + (key == Rml::Input::KI_NEXT ? viewport : -viewport);
            const int target = std::clamp(heights.indexAt(std::max(0.0f, y)), 0, std::max(0, heights.count() - 1)) * perLine +
                               std::max(0, current) % perLine;
            moveActive(target, shift);
            return true;
        }
        case Rml::Input::KI_HOME:
            moveActive(0, shift);
            return true;
        case Rml::Input::KI_END:
            moveActive(count - 1, shift);
            return true;
        case Rml::Input::KI_A:
            if (ctrl && selectionMode == SelectionMode::Multiple){
                selectRange(0, count - 1, false);
                for (Row& row : pool){
                    if (row.item >= 0)
                        setRowClasses(row);
                }
                notifySelection();
                return true;
            }
            return false;
        case Rml::Input::KI_RETURN:
        case Rml::Input::KI_NUMPADENTER:
            if (current >= 0){
                activate(current);
                return true;
            }
            return false;
        case Rml::Input::KI_SPACE:
            if (current >= 0 && selectionMode != SelectionMode::None){
                selectFromPointer(current, selectionMode == SelectionMode::Multiple, false);
                return true;
            }
            return false;
        default:
            return false;
    }
}

void ItemsViewElement::ProcessDefaultAction(Rml::Event& event){
    Rml::Element::ProcessDefaultAction(event);
    if (kind == Kind::Table && event == Rml::EventId::Click && handleHeaderClick(event))
        return;

    switch (event.GetId()){
        case Rml::EventId::Keydown:
            if (handleKey(event))
                event.StopPropagation();
            break;
        case Rml::EventId::Click:{
            Rml::Element* target = event.GetTargetElement();
            const int index = itemOfElement(target);
            if (index < 0)
                break;
            if (kind == Kind::Tree){
                for (Rml::Element* each = target; each && !isRowElement(each); each = each->GetParentNode()){
                    if (each->HasAttribute("tree-toggle")){
                        toggle(keyOf(index));
                        event.StopPropagation();
                        return;
                    }
                }
            }
            const bool ctrl = event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0;
            const bool shift = event.GetParameter<int>("shift_key", 0) != 0;
            active = index;
            selectFromPointer(index, ctrl, shift);
            break;
        }
        case Rml::EventId::Dblclick:{
            const int index = itemOfElement(event.GetTargetElement());
            if (index < 0)
                break;
            if (kind == Kind::Tree && nodeAt(index).hasChildren)
                toggle(keyOf(index));
            activate(index);
            break;
        }
        case Rml::EventId::Focus:
            // the view's tab stop gives the focus to the active row, at the next update: a focus
            // change from inside this one would send the view its focus event again (the context
            // commits the new focus chain after its events), without end
            if (event.GetTargetElement() == this && active >= 0 && rowOf(active))
                focusRowFromView = true;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- live patch

void ItemsViewElement::onDocumentPatched(const std::unordered_set<int>& changedTemplates){
    if (!structureReady)
        return;
    auto changed = [&](Rml::Element* element){
        if (!element)
            return false;
        const Rml::Variant* index = element->GetAttribute(compat::LuaHooks::kTemplateAttribute);
        return index && changedTemplates.count(std::atoi(index->Get<Rml::String>().c_str())) > 0;
    };
    bool remake = changed(templateElement);
    for (Column& column : columns){
        remake |= changed(column.cellTemplate);
        column.cellRml.clear();
    }
    rowRml.clear();
    if (remake){
        for (Row& row : pool)
            clearRowContent(row);
        rebindAll = true;
    }
    windowDirty = true;
    if (Rml::Context* context = GetContext())
        context->RequestNextUpdate(0);
}

// ---------------------------------------------------------------- state

ItemsViewElement::State ItemsViewElement::getState() const{
    State state;
    state.kind = kind;
    state.count = (kind == Kind::Tree) ? (int)nodes.size() : itemCount;
    const std::pair<int, int> range = getVisibleRange();
    state.first = range.first;
    state.last = range.second;
    state.pool = (int)pool.size();
    const float minHeight = std::max(kMinLineHeight, std::min(heights.estimate(), minLineHeight > 0.0f ? minLineHeight : heights.estimate()));
    // (+1: a line cut at the top and one at the bottom show together)
    state.poolBound = ((int)std::ceil(viewportHeight() / minHeight) + 1 + 2 * overscan + VirtualWindowCore::kShrinkSlackLines) * std::max(1, lanes);
    state.overscan = overscan;
    state.lanes = lanes;
    state.selection = selectionMode;
    state.selected = getSelection();
    for (int index : state.selected)
        state.selectedKeys.push_back(keyOf(index));
    state.active = active;
    auto* self = const_cast<ItemsViewElement*>(this);
    state.scrollTop = self->GetScrollTop();
    state.totalHeight = heights.total();
    if (model)
        state.source = "model:" + modelName;
    else if (providerSet)
        state.source = "lua";
    state.layoutBoundary = boundary;
    state.rowAliases = useRowAliases() && model;
    size_t bytes = heights.memoryBytes();
    for (const std::string& key : selectedKeys)
        bytes += sizeof(std::string) + key.capacity() + 16;
    bytes += nodes.capacity() * sizeof(TreeNodeInfo);
    for (const std::vector<int>& path : nodePaths)
        bytes += path.capacity() * sizeof(int);
    state.memoryBytes = bytes;
    return state;
}

// ---------------------------------------------------------------- instancer

Rml::ElementPtr ItemsViewInstancer::InstanceElement(Rml::Element*, const Rml::String& tag, const Rml::XMLAttributes&){
    ItemsViewElement::Kind kind = ItemsViewElement::Kind::List;
    if (tag == "grid-view")
        kind = ItemsViewElement::Kind::Grid;
    else if (tag == "tree-view")
        kind = ItemsViewElement::Kind::Tree;
    else if (tag == "table-view")
        kind = ItemsViewElement::Kind::Table;
    return Rml::ElementPtr(new ItemsViewElement(tag, kind));
}

void ItemsViewInstancer::ReleaseElement(Rml::Element* element){
    delete element;
}
