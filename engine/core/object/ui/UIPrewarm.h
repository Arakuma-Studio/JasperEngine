// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UIPREWARM_H
#define UIPREWARM_H

#include "Export.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine{

    struct HtmlUIPrewarmState;   // htmlui/HtmlUIPrewarm.h

    // A text HtmlUI.prewarmText makes ready: the font as CSS names it, the effects as font-effect
    struct UIPrewarmText {
        std::string family = "sans-serif";   // a font-family list
        int size = 16;                       // px
        int weight = 400;
        bool italic = false;
        std::string effect;                  // a font-effect value ("outline(15px #000)"), empty: none
        std::string text;
    };

    // S93 R15: a prewarm in flight (UIDocument::prewarm, UIPrewarm::prewarmText, prewarmImages). The glyphs of what will
    // be shown (base, distance and effect glyphs) are made on background jobs and placed in the glyph atlas within
    // getBudgetMs() a frame, its images decoded and uploaded likewise, so the frame that first shows them makes nothing.
    class JASPERENGINE_API UIPrewarm {
    public:
        UIPrewarm();   // done, nothing asked for
        explicit UIPrewarm(std::shared_ptr<HtmlUIPrewarmState> state);

        bool isDone() const;
        float getProgress() const;   // 0..1, 1 once done

        // the glyphs of a text in every context drawing now (the default scale before any)
        static UIPrewarm prewarmText(const UIPrewarmText& text);
        // images decoded in the background and uploaded a few a frame (asset paths as CSS names them, #xywh= and #sprite=
        // regions too); they stay cached while the prewarm lives
        static UIPrewarm prewarmImages(const std::vector<std::string>& paths);
        // the main thread's time per frame for placing prewarmed glyphs, ms (default 2)
        static void setBudgetMs(double milliseconds);
        static double getBudgetMs();

        const std::shared_ptr<HtmlUIPrewarmState>& getState() const;

    private:
        std::shared_ptr<HtmlUIPrewarmState> state;
    };

}

#endif //UIPREWARM_H
