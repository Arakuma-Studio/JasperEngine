// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIPREWARM_H
#define HTMLUIPREWARM_H

// Engine-internal (RmlUi types): S93 R15, doc:prewarm and HtmlUI.prewarmText.

#include "HtmlUIImageCache.h"
#include "RmlFontEngine.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "object/ui/UIPrewarm.h"

#include <memory>
#include <string>
#include <vector>

namespace Rml {
    class ElementDocument;
}

namespace jasperengine {

    // What a UIPrewarm handle reads, and the Lua function called once it is done: its glyphs, and its images (P4's job:
    // decoded in the background, uploaded a few a frame, cached while the state holds them)
    struct HtmlUIPrewarmState {
        std::shared_ptr<const RmlFontEngine::GlyphPrewarm> glyphs;
        std::shared_ptr<HtmlUIImageCache::PrewarmJob> images;
        HtmlUILuaRef onDone;
        bool notified = false;   // onDone ran (or there was none when it was done)

        size_t total() const;
        size_t ready() const;
        bool done() const;
    };

    class JASPERENGINE_INTERNAL HtmlUIPrewarm {
    public:
        // every text of the document and every data-warm attribute, hidden and display: none subtrees included (their
        // styles are resolved), with each element's font and font-effect, at its context's raster scale; with images, the
        // raster images of its <img> elements and of the files its compile read (style sheets' images)
        static std::shared_ptr<HtmlUIPrewarmState> prewarmDocument(Rml::ElementDocument* document, bool images);
        // asset paths as CSS names them (#xywh= and #sprite= regions too): HtmlUIImageCache::prewarm
        static std::shared_ptr<HtmlUIPrewarmState> prewarmImages(const std::vector<std::string>& paths);
        // error set (and a done state) when the effect does not parse
        static std::shared_ptr<HtmlUIPrewarmState> prewarmText(const UIPrewarmText& text, std::string* error);
        // once per frame (UIDocumentSystem::update, every scene calls it): places prewarmed glyphs within the budget, then
        // calls onDone of what is done
        static void update();
        // keeps the state until it is done and its onDone ran (the Lua handle may be dropped before)
        static void track(const std::shared_ptr<HtmlUIPrewarmState>& state);

        static void setBudgetMs(double milliseconds);
        static double getBudgetMs();
        // the main thread's time in the last update's placement, ms (tests: the budget)
        static double getLastUpdateMs();
    };

}

#endif //HTMLUIPREWARM_H
