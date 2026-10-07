// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "object/ui/UIElement.h"
#include "htmlui/HtmlUIContext.h"

using namespace jasperengine;

UIDocumentSystem::UIDocumentSystem(Scene* scene): SubSystem(scene){
    // HTMLUI_SKELETON: p0-ecs
}

UIDocumentSystem::~UIDocumentSystem(){
    // HTMLUI_SKELETON: p0-ecs
}

void UIDocumentSystem::load(){
    // HTMLUI_SKELETON: p0-ecs
}

void UIDocumentSystem::destroy(){
    // HTMLUI_SKELETON: p0-ecs
}

void UIDocumentSystem::update(double dt){
    // HTMLUI_SKELETON: p0-ecs
    (void)dt;
}

void UIDocumentSystem::draw(){
    // HTMLUI_SKELETON: p0-ecs
}

void UIDocumentSystem::onComponentAdded(Entity entity, ComponentId componentId){
    // HTMLUI_SKELETON: p0-ecs
    (void)entity;
    (void)componentId;
}

void UIDocumentSystem::onComponentRemoved(Entity entity, ComponentId componentId){
    // HTMLUI_SKELETON: p0-ecs
    (void)entity;
    (void)componentId;
}

void UIDocumentSystem::renderOffscreen(const Rect& viewRect){
    if (context) context->render(viewRect);
}

void UIDocumentSystem::drawComposite(PipelineType colorPipeline, bool flipY){
    if (context) context->composite(colorPipeline, flipY);
}

bool UIDocumentSystem::eventOnPointerDown(int button, float x, float y, int mods){
    // HTMLUI_SKELETON: p0-ecs
    (void)button;
    (void)x;
    (void)y;
    (void)mods;
    return false;
}

bool UIDocumentSystem::eventOnPointerUp(int button, float x, float y, int mods){
    // HTMLUI_SKELETON: p0-ecs
    (void)button;
    (void)x;
    (void)y;
    (void)mods;
    return false;
}

bool UIDocumentSystem::eventOnPointerMove(float x, float y, int mods){
    // HTMLUI_SKELETON: p0-ecs
    (void)x;
    (void)y;
    (void)mods;
    return false;
}

void UIDocumentSystem::setReferenceSize(int width, int height){
    // HTMLUI_SKELETON: p0-ecs
    (void)width;
    (void)height;
}

void UIDocumentSystem::clearReferenceSize(){
    // HTMLUI_SKELETON: p0-ecs
}

bool UIDocumentSystem::loadDocumentNow(Entity entity){
    // HTMLUI_SKELETON: p0-ecs
    (void)entity;
    return false;
}

bool UIDocumentSystem::isDocumentLoaded(Entity entity) const{
    // HTMLUI_SKELETON: p0-ecs
    (void)entity;
    return false;
}

UIElement UIDocumentSystem::getElementById(Entity entity, const std::string& id) const{
    // HTMLUI_SKELETON: p0-ecs
    (void)entity;
    (void)id;
    return UIElement();
}

UIElement UIDocumentSystem::getDocumentBody(Entity entity) const{
    // HTMLUI_SKELETON: p0-ecs
    (void)entity;
    return UIElement();
}

bool UIDocumentSystem::hasDocuments() const{
    // HTMLUI_SKELETON: p0-ecs
    return false;
}

bool UIDocumentSystem::isAllLoaded() const{
    // HTMLUI_SKELETON: p0-ecs
    return true;
}

void UIDocumentSystem::setDebuggerVisible(bool visible){
    // HTMLUI_SKELETON: p0-ecs
    (void)visible;
}

HtmlUIContext* UIDocumentSystem::getContext() const{
    // HTMLUI_SKELETON: p0-ecs
    return nullptr;
}
