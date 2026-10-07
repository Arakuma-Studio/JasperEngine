// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENT_H
#define UIDOCUMENT_H

#include "GameObject.h"
#include "UIElement.h"
#include "UIEvent.h"
#include "UIDataModel.h"
#include "UIValue.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine{

    class Framebuffer;

    // A GameObject showing an HTML/CSS document over the scene (UIDocumentComponent)
    class JASPERENGINE_API UIDocument: public GameObject {
    public:
        UIDocument(Scene* scene);                 // adds UIDocumentComponent
        UIDocument(Scene* scene, Entity entity);
        virtual ~UIDocument();

        // --- phase 0 ---
        void setFilename(const std::string& filename);   // loads at the next update
        const std::string& getFilename() const;
        bool load(const std::string& filename);          // setFilename + UIDocumentSystem::loadDocumentNow; false if it failed
        void reload();                                   // needReload = true (phase 1: keeps state, §3.8.6)
        bool isLoaded() const;

        void show(); void hide();
        void setVisible(bool visible); bool isVisible() const;   // component.enabled
        void setSortOrder(int sortOrder); int getSortOrder() const;

        UIElement getElementById(const std::string& id) const;
        UIElement getBody() const;

        // --- phase 1 (p1-lua) ---
        UIElement querySelector(const std::string&) const;  std::vector<UIElement> querySelectorAll(const std::string&) const;
        UIElement createElement(const std::string& tag) const;  UIElement createTextNode(const std::string&) const;
        std::string getTitle() const; void setTitle(const std::string&);
        uint64_t addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture = false); // on the document element
        void removeEventListener(uint64_t);
        UIDataModel createDataModel(const std::string& name, const UIValue& initial);  // object; top-level keys fixed
        UIDataModel getDataModel(const std::string& name) const;
        void removeDataModel(const std::string& name);
        static void setDebuggerVisible(bool); static bool isDebuggerVisible();   // the main scene's UIDocumentSystem

        // --- phase 1 (p1-rtt) ---
        Framebuffer* getTargetTexture() const;           // TEXTURE mode; nullptr otherwise (UIDocumentSystem::getTargetTexture)
    };

}

#endif //UIDOCUMENT_H
