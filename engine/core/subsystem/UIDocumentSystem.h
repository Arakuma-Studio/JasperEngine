// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENTSYSTEM_H
#define UIDOCUMENTSYSTEM_H

#include "SubSystem.h"
#include "math/Rect.h"
#include "render/Render.h"
#include "component/UIDocumentComponent.h"

#include <memory>
#include <string>
#include <unordered_map>

// RmlUi stays out of the engine API (swappable core): forward declarations only
namespace Rml {
    class ElementDocument;
}

namespace jasperengine{

    class HtmlUIContext;
    class UIElement;

    // The HTML/CSS documents of a scene (UIDocumentComponent): one RmlUi context, created
    // the first time the scene has a document, drawn offscreen and composited in front of
    // the component UI by RenderSystem.
    class JASPERENGINE_API UIDocumentSystem : public SubSystem {
    public:
        UIDocumentSystem(Scene* scene);
        virtual ~UIDocumentSystem();

        void load() override;                 // nothing heavy: the context is created lazily in update()
        void destroy() override;              // destroy context, HtmlUI::release() if acquired
        void update(double dt) override;
        void draw() override;                 // empty: RenderSystem drives drawing
        void onComponentAdded(Entity entity, ComponentId componentId) override;    // mark needReload
        void onComponentRemoved(Entity entity, ComponentId componentId) override;  // unload that document

        // RenderSystem hooks; one-line delegations to the context
        void renderOffscreen(const Rect& viewRect);                 // context->render(viewRect)
        void drawComposite(PipelineType colorPipeline, bool flipY); // context->composite(...)

        // Engine::system* hooks, canvas coordinates (top-left origin, y down); true = consumed
        bool eventOnPointerDown(int button, float x, float y, int mods);
        bool eventOnPointerUp(int button, float x, float y, int mods);
        bool eventOnPointerMove(float x, float y, int mods);

        // the editor lays documents out at the project canvas whatever the view size
        // (mirrors UISystem::setAnchorReferenceSize)
        void setReferenceSize(int width, int height);
        void clearReferenceSize();

        bool loadDocumentNow(Entity entity);           // UIDocument::load: synchronous when the view is loaded
        bool isDocumentLoaded(Entity entity) const;
        UIElement getElementById(Entity entity, const std::string& id) const;
        UIElement getDocumentBody(Entity entity) const;

        bool hasDocuments() const;      // any UIDocumentComponent with a filename
        bool isAllLoaded() const;       // every enabled document loaded or failed, and (if any visible) a frame rendered
        void setDebuggerVisible(bool visible);
        HtmlUIContext* getContext() const;   // internal/tests

    private:
        std::unique_ptr<HtmlUIContext> context;
        std::unordered_map<Entity, Rml::ElementDocument*> documents;
        bool acquired = false;
        int referenceWidth = 0, referenceHeight = 0;
        bool pointerCaptured = false;   // a consumed down is followed by a consumed up
    };

}

#endif //UIDOCUMENTSYSTEM_H
