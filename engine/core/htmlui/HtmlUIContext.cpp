// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIContext.h"

#include "RmlRenderer.h"

using namespace jasperengine;

HtmlUIContext::HtmlUIContext(const std::string& name): name(name), renderer(new RmlRenderer()){
    // HTMLUI_SKELETON: p0-ecs
}

HtmlUIContext::~HtmlUIContext(){
    // HTMLUI_SKELETON: p0-ecs
}

bool HtmlUIContext::create(Rml::Vector2i dimensions){
    // HTMLUI_SKELETON: p0-ecs
    (void)dimensions;
    return false;
}

void HtmlUIContext::destroy(){
    // HTMLUI_SKELETON: p0-ecs
}

Rml::Context* HtmlUIContext::getRmlContext() const{
    // HTMLUI_SKELETON: p0-ecs
    return nullptr;
}

void HtmlUIContext::setDimensions(Rml::Vector2i dimensions){
    // HTMLUI_SKELETON: p0-ecs
    (void)dimensions;
}

Rml::Vector2i HtmlUIContext::getDimensions() const{
    // HTMLUI_SKELETON: p0-ecs
    return Rml::Vector2i(0, 0);
}

void HtmlUIContext::setDensityRatio(float ratio){
    // HTMLUI_SKELETON: p0-ecs
    (void)ratio;
}

Rml::ElementDocument* HtmlUIContext::loadDocument(const std::string& path){
    // HTMLUI_SKELETON: p0-ecs
    (void)path;
    return nullptr;
}

void HtmlUIContext::unloadDocument(Rml::ElementDocument* document){
    // HTMLUI_SKELETON: p0-ecs
    (void)document;
}

void HtmlUIContext::update(){
    // HTMLUI_SKELETON: p0-ecs
}

bool HtmlUIContext::processMouseMove(Rml::Vector2f position, int modifiers){
    // HTMLUI_SKELETON: p0-ecs
    (void)position;
    (void)modifiers;
    return false;
}

bool HtmlUIContext::processMouseButtonDown(int button, int modifiers){
    // HTMLUI_SKELETON: p0-ecs
    (void)button;
    (void)modifiers;
    return false;
}

bool HtmlUIContext::processMouseButtonUp(int button, int modifiers){
    // HTMLUI_SKELETON: p0-ecs
    (void)button;
    (void)modifiers;
    return false;
}

bool HtmlUIContext::processMouseLeave(){
    // HTMLUI_SKELETON: p0-ecs
    return false;
}

bool HtmlUIContext::isPointerOverContent() const{
    // HTMLUI_SKELETON: p0-ecs
    return false;
}

void HtmlUIContext::setDebuggerVisible(bool visible){
    // HTMLUI_SKELETON: p0-ecs
    (void)visible;
}
