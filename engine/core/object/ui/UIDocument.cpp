// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIDocument.h"

#include "subsystem/UIDocumentSystem.h"

using namespace jasperengine;

UIDocument::UIDocument(Scene* scene): GameObject(scene){
    // HTMLUI_SKELETON: p0-ecs
}

UIDocument::UIDocument(Scene* scene, Entity entity): GameObject(scene, entity){
    // HTMLUI_SKELETON: p0-ecs
}

UIDocument::~UIDocument(){
    // HTMLUI_SKELETON: p0-ecs
}

void UIDocument::setFilename(const std::string& filename){
    // HTMLUI_SKELETON: p0-ecs
    (void)filename;
}

const std::string& UIDocument::getFilename() const{
    // HTMLUI_SKELETON: p0-ecs
    static const std::string empty;
    return empty;
}

bool UIDocument::load(const std::string& filename){
    // HTMLUI_SKELETON: p0-ecs
    (void)filename;
    return false;
}

void UIDocument::reload(){
    // HTMLUI_SKELETON: p0-ecs
}

bool UIDocument::isLoaded() const{
    // HTMLUI_SKELETON: p0-ecs
    return false;
}

void UIDocument::show(){
    // HTMLUI_SKELETON: p0-ecs
}

void UIDocument::hide(){
    // HTMLUI_SKELETON: p0-ecs
}

void UIDocument::setVisible(bool visible){
    // HTMLUI_SKELETON: p0-ecs
    (void)visible;
}

bool UIDocument::isVisible() const{
    // HTMLUI_SKELETON: p0-ecs
    return false;
}

void UIDocument::setSortOrder(int sortOrder){
    // HTMLUI_SKELETON: p0-ecs
    (void)sortOrder;
}

int UIDocument::getSortOrder() const{
    // HTMLUI_SKELETON: p0-ecs
    return 0;
}

UIElement UIDocument::getElementById(const std::string& id) const{
    // HTMLUI_SKELETON: p0-ecs
    (void)id;
    return UIElement();
}

UIElement UIDocument::getBody() const{
    // HTMLUI_SKELETON: p0-ecs
    return UIElement();
}
