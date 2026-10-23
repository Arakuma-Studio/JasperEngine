// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// <tree-view> (S17 R10): the expanded structure flattened into the view's rows. In model mode the
// items are the roots, `children-key` (default "children") names each node's children and `key`
// a field that identifies a node (default: its index path, "2.0.3"); a Lua provider names a
// node's children by key. Only expanded nodes are walked, so a structural change costs the
// visible structure.

#include "htmlui/widgets/ItemsViewElement.h"

#include "object/ui/HtmlUIEventListener.h"
#include "object/ui/UIValue.h"

#include "../../../libs/rmlui/Source/Core/DataModel.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataVariable.h>
#include <RmlUi/Core/Variant.h>

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    std::string variantKey(const Rml::Variant& value){
        if (value.GetType() == Rml::Variant::DOUBLE || value.GetType() == Rml::Variant::FLOAT){
            const double number = value.Get<double>();
            if (std::floor(number) == number && std::abs(number) < 1e15)
                return std::to_string((long long)number);
        }
        return value.Get<Rml::String>();
    }

}

Rml::DataAddress ItemsViewElement::treeAddress(int index) const{
    Rml::DataAddress address = containerAddress;
    if (index < 0 || index >= (int)nodePaths.size())
        return address;
    const std::vector<int>& path = nodePaths[index];
    for (size_t depth = 0; depth < path.size(); depth++){
        if (depth > 0)
            address.emplace_back(childrenKey);
        address.emplace_back(path[depth]);
    }
    return address;
}

void ItemsViewElement::flattenChildren(int parentFlat, const std::string& parentKey, int depth, const Rml::DataAddress* parentAddress){
    if (providerSet){
        if (!provider.children)
            return;
        for (const std::string& key : provider.children(parentKey)){
            TreeNodeInfo node;
            node.key = key;
            node.depth = depth;
            node.parent = parentFlat;
            node.hasChildren = provider.hasChildren ? provider.hasChildren(key) : !provider.children(key).empty();
            node.expanded = node.hasChildren && expandedKeys.count(key) > 0;
            nodes.push_back(node);
            if (node.expanded)
                flattenChildren((int)nodes.size() - 1, key, depth + 1, nullptr);
        }
        return;
    }
    if (!model || !parentAddress)
        return;

    // the roots are the items; a node's children its children-key array
    Rml::DataAddress listAddress = *parentAddress;
    if (parentFlat >= 0)
        listAddress.emplace_back(childrenKey);
    Rml::DataVariable list = model->GetVariable(listAddress);
    const int count = list ? list.Size() : 0;
    for (int i = 0; i < count; i++){
        Rml::DataAddress address = listAddress;
        address.emplace_back(i);
        Rml::DataVariable variable = model->GetVariable(address);

        TreeNodeInfo node;
        node.depth = depth;
        node.parent = parentFlat;
        std::vector<int> path = parentFlat >= 0 ? nodePaths[parentFlat] : std::vector<int>();
        path.push_back(i);
        if (!keyField.empty()){
            Rml::Variant value;
            Rml::DataVariable field = variable ? variable.Child(Rml::DataAddressEntry(keyField)) : Rml::DataVariable();
            if (field && field.Get(value))
                node.key = variantKey(value);
        }
        if (node.key.empty()){
            for (size_t k = 0; k < path.size(); k++)
                node.key += (k ? "." : "") + std::to_string(path[k]);
        }
        Rml::DataVariable children = variable ? variable.Child(Rml::DataAddressEntry(childrenKey)) : Rml::DataVariable();
        node.hasChildren = children && children.Size() > 0;
        node.expanded = node.hasChildren && expandedKeys.count(node.key) > 0;
        nodes.push_back(node);
        nodePaths.push_back(std::move(path));
        if (node.expanded)
            flattenChildren((int)nodes.size() - 1, nodes.back().key, depth + 1, &address);
    }
}

void ItemsViewElement::rebuildTree(){
    treeDirty = false;
    const std::string activeKey = (active >= 0 && active < (int)nodes.size()) ? nodes[active].key : std::string();
    nodes.clear();
    nodePaths.clear();
    if (providerSet)
        flattenChildren(-1, std::string(), 0, nullptr);
    else if (model)
        flattenChildren(-1, std::string(), 0, &containerAddress);
    itemCount = (int)nodes.size();
    if (!activeKey.empty())
        active = indexOfKey(activeKey);
    heightChecked = false;
    windowDirty = true;
}

ItemsViewElement::TreeNodeInfo ItemsViewElement::nodeAt(int index) const{
    if (index < 0 || index >= (int)nodes.size())
        return TreeNodeInfo();
    return nodes[index];
}

bool ItemsViewElement::isExpanded(const std::string& key) const{
    return expandedKeys.count(key) > 0;
}

bool ItemsViewElement::expand(const std::string& key){
    if (kind != Kind::Tree)
        return false;
    const int index = indexOfKey(key);
    if (index < 0 || !nodes[index].hasChildren || nodes[index].expanded)
        return false;
    expandedKeys.insert(key);
    rebuildTree();
    rebindAll = true;
    updateWindow(VirtualizationCoordinator::Reason::DataChanged, false);
    dispatchDetail("expand", index);
    return true;
}

bool ItemsViewElement::collapse(const std::string& key){
    if (kind != Kind::Tree)
        return false;
    const int index = indexOfKey(key);
    if (index < 0 || !nodes[index].expanded)
        return false;
    // the active node inside the collapsed one becomes the collapsed one
    if (active > index){
        int ancestor = active;
        while (ancestor >= 0 && ancestor != index)
            ancestor = nodes[ancestor].parent;
        if (ancestor == index)
            active = index;
    }
    expandedKeys.erase(key);
    rebuildTree();
    rebindAll = true;
    updateWindow(VirtualizationCoordinator::Reason::DataChanged, false);
    dispatchDetail("collapse", indexOfKey(key));
    return true;
}

bool ItemsViewElement::toggle(const std::string& key){
    return isExpanded(key) ? collapse(key) : expand(key);
}
