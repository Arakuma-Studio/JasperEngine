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
        bool load(const std::string& filename);          // loads now (when the view is loaded); false if it failed
        void reload();                                   // loads it again, keeping scroll, focus, form values and id-ed listeners
        bool isLoaded() const;

        void show(); void hide();
        void setVisible(bool visible); bool isVisible() const;   // component.enabled
        void setSortOrder(int sortOrder); int getSortOrder() const;   // higher draws on top

        UIElement getElementById(const std::string& id) const;
        UIElement getBody() const;                       // the document element (RML body)

        // --- phase 1 (p1-lua) ---

        UIElement querySelector(const std::string& selector) const;
        std::vector<UIElement> querySelectorAll(const std::string& selector) const;
        // a detached element: insert it with appendChild/insertBefore
        UIElement createElement(const std::string& tag) const;
        UIElement createTextNode(const std::string& text) const;
        std::string getTitle() const; void setTitle(const std::string& title);
        // on the document element; returns an id for removeEventListener (0 if not added)
        uint64_t addEventListener(const std::string& type, std::function<void(UIEvent&)> fn, bool capture = false);
        void removeEventListener(uint64_t listenerId);
        // initial is an object whose top-level keys are fixed; a loaded document that uses the
        // model (data-model="name") reloads; create data models first in init
        UIDataModel createDataModel(const std::string& name, const UIValue& initial);
        UIDataModel getDataModel(const std::string& name) const;   // one this document created
        void removeDataModel(const std::string& name);
        // the RmlUi debugger on the main scene's documents
        static void setDebuggerVisible(bool visible); static bool isDebuggerVisible();

        // --- phase 1 (p1-rtt) ---

        Framebuffer* getTargetTexture() const;           // TEXTURE mode target; nullptr otherwise (UIDocumentSystem::getTargetTexture)
    };

}

#endif //UIDOCUMENT_H
