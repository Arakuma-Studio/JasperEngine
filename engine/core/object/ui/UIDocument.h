// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIDOCUMENT_H
#define UIDOCUMENT_H

#include "GameObject.h"
#include "UIElement.h"
#include "UIEvent.h"
#include "UIDataModel.h"
#include "UIReloadReport.h"
#include "UIValue.h"
#include "htmlui/compat/Diagnostics.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine{

    class Framebuffer;

    // The tooltips of a document (S17 R16): shown at all, after `delay` seconds of hover or
    // focus, at once on another source within `hopWindow` seconds of the last one
    struct UITooltipConfig {
        bool enabled = true;
        float delay = 0.5f;
        float hopWindow = 0.5f;
    };

    // The tooltip a document shows (inspect_ui)
    struct UITooltipInfo {
        bool visible = false;
        UIElement source;
        UIElement tooltip;           // div.htmlui-tooltip
        std::string text;
        UIRect rect{};               // document px
        std::string side;            // "top", "bottom", "left", "right"
    };

    // A GameObject showing an HTML/CSS document over the scene (UIDocumentComponent)
    class JASPERENGINE_API UIDocument: public GameObject {
    public:
        UIDocument(Scene* scene);                 // adds UIDocumentComponent
        UIDocument(Scene* scene, Entity entity);
        virtual ~UIDocument();

        UIDocument(const UIDocument& rhs);
        UIDocument& operator=(const UIDocument& rhs);
        UIDocument(UIDocument&& rhs) noexcept;
        UIDocument& operator=(UIDocument&& rhs) noexcept;

        // --- phase 0 ---

        void setFilename(const std::string& filename);   // loads at the next update
        const std::string& getFilename() const;
        bool load(const std::string& filename);          // loads now (when the view is loaded); false if it failed
        void reload();                                   // loads it again, keeping scroll, focus, form values and id-ed listeners
        // S17: an .html document is patched in place (UIReloadMode::PATCH: every element, listener and
        // handle that is still in it stays) or loaded again (REBUILD, as reload() before)
        void reload(UIReloadMode mode);
        // after a patch reload; the last reload's report
        void setOnReload(std::function<void(const UIReloadReport&)> callback);
        UIReloadReport getLastReloadReport() const;
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

        // --- phase 2 (p2-html, hook H4) ---
        // HTML documents: getBody() is the HTML <body>, this the <html> element (the RML body)
        UIElement getDocumentElement() const;
        // the compatibility layer's diagnostics of this document (compile time, RmlUi messages
        // mapped to the HTML/CSS source, Lua script errors); what check --ui reports for the file
        std::vector<htmlui::compat::Diagnostic> getDiagnostics() const;
        // prefers-color-scheme ("light" default, "dark") and prefers-reduced-motion of the
        // documents (per scene: every document of the scene's context follows)
        void setColorScheme(const std::string& scheme);
        void setReducedMotion(bool reduced);

        // --- phase 5 (p5-infra) ---
        // Freezes every UI effect at `seconds` of the effect clock (nullopt: they run on the UI
        // clock again). Effects only: CSS animations, Lottie and anime keep running. Integrated
        // clocks use their closed form, the pointer smoothing snaps: renders are deterministic.
        static void setEffectTime(std::optional<double> seconds);
        // the effect clock: the fixed time when one is set, else the UI clock
        static double getEffectTime();
        // phase 6: pinned stateful effects whose replay is still running (they draw once it ends,
        // a few frames later; screenshots wait for it)
        static size_t getPendingEffectReplays();

        // --- S17 ---
        void configureTooltips(const UITooltipConfig& config);
        UITooltipConfig getTooltipConfig() const;
        UITooltipInfo getTooltip() const;
        bool showTooltip(const UIElement& source);   // at once (false when it has none)
        void hideTooltip();
        // Markdown to the HTML <markdown> and setMarkdown show (div.markdown-body; HTML in it is text)
        static std::string markdownToHtml(const std::string& markdown);
    };

}

#endif //UIDOCUMENT_H
