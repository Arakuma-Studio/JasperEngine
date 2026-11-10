// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANIMTEXT_H
#define ANIMTEXT_H

#include "Export.h"
#include "AnimParams.h"
#include "object/ui/UIElement.h"

#include <memory>
#include <string>
#include <vector>

namespace Rml { class Context; }

namespace jasperengine {

    // Anime.js 4.5.0's TextSplitter: the returned elements are ordinary anime targets.
    // A layout refresh replaces the generated elements; retrieve the arrays again afterwards.
    class JASPERENGINE_API AnimText : public std::enable_shared_from_this<AnimText> {
    public:
        static std::shared_ptr<AnimText> create(const UIElement& target, const AnimParam& params = AnimParam::object());
        ~AnimText();

        const std::vector<UIElement>& getChars() const { return chars; }
        const std::vector<UIElement>& getWords() const { return words; }
        const std::vector<UIElement>& getLines() const { return lines; }
        UIElement getTarget() const { return target; }
        bool isReady() const { return ready; }

        AnimText& split();
        AnimText& refresh();
        AnimText& revert();

        // Called after layout, so line grouping uses the new, untransformed boxes.
        static bool updateLayouts(Rml::Context* context);

    private:
        AnimText(const UIElement& target, const AnimParam& params);
        void rebuild();
        void rememberLayout();
        bool layoutChanged() const;

        UIElement target;
        AnimParam params;
        std::string original;
        std::vector<UIElement> chars, words, lines;
        std::vector<float> layout;
        bool ready = false;
        bool active = true;
        bool rebuilding = false;
    };

}

#endif //ANIMTEXT_H
