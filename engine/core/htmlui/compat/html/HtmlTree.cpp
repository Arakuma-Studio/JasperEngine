// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/html/HtmlTree.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

namespace {

    const std::string emptyValue;

}

std::unique_ptr<HtmlNode> HtmlNode::makeElement(std::string tag, Namespace ns){
    std::unique_ptr<HtmlNode> node(new HtmlNode());
    node->kind = Kind::Element;
    node->tag = std::move(tag);
    node->ns = ns;
    node->implied = true;
    return node;
}

std::unique_ptr<HtmlNode> HtmlNode::makeText(std::string text){
    std::unique_ptr<HtmlNode> node(new HtmlNode());
    node->kind = Kind::Text;
    node->text = std::move(text);
    node->implied = true;
    return node;
}

std::unique_ptr<HtmlNode> HtmlNode::makeRaw(std::string markup){
    std::unique_ptr<HtmlNode> node(new HtmlNode());
    node->kind = Kind::Raw;
    node->text = std::move(markup);
    node->implied = true;
    return node;
}

const HtmlAttr* HtmlNode::findAttr(std::string_view name) const{
    for (const HtmlAttr& attribute : attrs){
        if (attribute.name == name)
            return &attribute;
    }
    return nullptr;
}

HtmlAttr* HtmlNode::findAttr(std::string_view name){
    for (HtmlAttr& attribute : attrs){
        if (attribute.name == name)
            return &attribute;
    }
    return nullptr;
}

const std::string& HtmlNode::attr(std::string_view name) const{
    const HtmlAttr* attribute = findAttr(name);
    return attribute ? attribute->value : emptyValue;
}

HtmlAttr& HtmlNode::setAttr(std::string_view name, std::string value){
    if (HtmlAttr* existing = findAttr(name)){
        existing->value = std::move(value);
        existing->hasValue = true;
        return *existing;
    }
    HtmlAttr attribute;
    attribute.name = std::string(name);
    attribute.value = std::move(value);
    attribute.hasValue = true;
    attrs.push_back(std::move(attribute));
    return attrs.back();
}

bool HtmlNode::removeAttr(std::string_view name){
    for (auto it = attrs.begin(); it != attrs.end(); ++it){
        if (it->name == name){
            attrs.erase(it);
            return true;
        }
    }
    return false;
}

HtmlNode* HtmlNode::appendChild(std::unique_ptr<HtmlNode> child){
    child->parent = this;
    children.push_back(std::move(child));
    return children.back().get();
}

HtmlNode* HtmlNode::insertChild(size_t index, std::unique_ptr<HtmlNode> child){
    child->parent = this;
    if (index > children.size())
        index = children.size();
    auto it = children.insert(children.begin() + (std::ptrdiff_t)index, std::move(child));
    return it->get();
}

std::unique_ptr<HtmlNode> HtmlNode::removeChild(const HtmlNode* child){
    for (auto it = children.begin(); it != children.end(); ++it){
        if (it->get() == child){
            std::unique_ptr<HtmlNode> removed = std::move(*it);
            children.erase(it);
            removed->parent = nullptr;
            return removed;
        }
    }
    return nullptr;
}

size_t HtmlNode::indexInParent() const{
    if (!parent)
        return 0;
    for (size_t i = 0; i < parent->children.size(); i++){
        if (parent->children[i].get() == this)
            return i;
    }
    return 0;
}

void HtmlNode::unwrap(){
    HtmlNode* owner = parent;
    if (!owner)
        return;
    const size_t index = indexInParent();
    std::vector<std::unique_ptr<HtmlNode>> moved = std::move(children);
    children.clear();
    std::unique_ptr<HtmlNode> self = owner->removeChild(this);
    size_t at = index;
    for (std::unique_ptr<HtmlNode>& child : moved){
        owner->insertChild(at++, std::move(child));
    }
    // self (this) is destroyed here
}

SourceRef HtmlNode::location() const{
    for (const HtmlNode* walk = this; walk; walk = walk->parent){
        if (walk->ref.valid())
            return walk->ref;
    }
    return SourceRef{};
}

std::string HtmlNode::textContent() const{
    std::string out;
    if (kind == Kind::Text){
        out = text;
        return out;
    }
    for (const std::unique_ptr<HtmlNode>& child : children){
        if (child->kind == Kind::Text)
            out += child->text;
        else if (child->kind == Kind::Element)
            out += child->textContent();
    }
    return out;
}
