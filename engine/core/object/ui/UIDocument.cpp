// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIDocument.h"

#include "subsystem/UIDocumentSystem.h"

using namespace jasperengine;

UIDocument::UIDocument(Scene* scene): GameObject(scene){
    addComponent<UIDocumentComponent>();
}

UIDocument::UIDocument(Scene* scene, Entity entity): GameObject(scene, entity){
}

UIDocument::~UIDocument(){
}

void UIDocument::setFilename(const std::string& filename){
    UIDocumentComponent& document = getComponent<UIDocumentComponent>();

    // the same file again only retries a load that failed (reload() reloads a loaded one)
    if (document.filename != filename || document.loadFailed){
        document.filename = filename;
        document.needReload = true;
    }
}

const std::string& UIDocument::getFilename() const{
    return getComponent<UIDocumentComponent>().filename;
}

bool UIDocument::load(const std::string& filename){
    UIDocumentComponent& document = getComponent<UIDocumentComponent>();
    document.filename = filename;
    document.needReload = true;

    return scene->getSystem<UIDocumentSystem>()->loadDocumentNow(entity);
}

void UIDocument::reload(){
    getComponent<UIDocumentComponent>().needReload = true;
}

bool UIDocument::isLoaded() const{
    return getComponent<UIDocumentComponent>().loaded;
}

void UIDocument::show(){
    setVisible(true);
}

void UIDocument::hide(){
    setVisible(false);
}

void UIDocument::setVisible(bool visible){
    getComponent<UIDocumentComponent>().enabled = visible;
}

bool UIDocument::isVisible() const{
    return getComponent<UIDocumentComponent>().enabled;
}

void UIDocument::setSortOrder(int sortOrder){
    getComponent<UIDocumentComponent>().sortOrder = sortOrder;
}

int UIDocument::getSortOrder() const{
    return getComponent<UIDocumentComponent>().sortOrder;
}

UIElement UIDocument::getElementById(const std::string& id) const{
    return scene->getSystem<UIDocumentSystem>()->getElementById(entity, id);
}

UIElement UIDocument::getBody() const{
    return scene->getSystem<UIDocumentSystem>()->getDocumentBody(entity);
}

Framebuffer* UIDocument::getTargetTexture() const{
    return scene->getSystem<UIDocumentSystem>()->getTargetTexture(entity);
}
