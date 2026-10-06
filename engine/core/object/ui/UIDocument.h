// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENT_H
#define UIDOCUMENT_H

#include "GameObject.h"
#include "UIElement.h"

#include <string>

namespace jasperengine{

    // A GameObject showing an HTML/CSS document over the scene (UIDocumentComponent)
    class JASPERENGINE_API UIDocument: public GameObject {
    public:
        UIDocument(Scene* scene);                 // adds UIDocumentComponent
        UIDocument(Scene* scene, Entity entity);
        virtual ~UIDocument();

        void setFilename(const std::string& filename);   // loads at the next update
        const std::string& getFilename() const;
        bool load(const std::string& filename);          // setFilename + UIDocumentSystem::loadDocumentNow; false if it failed
        void reload();                                   // needReload = true
        bool isLoaded() const;

        void show(); void hide();
        void setVisible(bool visible); bool isVisible() const;   // component.enabled
        void setSortOrder(int sortOrder); int getSortOrder() const;

        UIElement getElementById(const std::string& id) const;
        UIElement getBody() const;
    };

}

#endif //UIDOCUMENT_H
