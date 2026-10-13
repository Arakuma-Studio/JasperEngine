// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_RUNTIME_HTMLINPUTELEMENT_H
#define COMPAT_RUNTIME_HTMLINPUTELEMENT_H

#include "Export.h"
#include "htmlui/compat/runtime/DecoratorCommon.h"

#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Geometry.h>

#include <memory>
#include <string>
#include <string_view>

namespace jasperengine::htmlui::compat {

    // <input> with the HTML types (phase-2 §3.11.2, Appendix D.3). The `type` attribute stays
    // as written (input[type=email] selectors keep matching); RmlUi gets the mapped type:
    // - search, email, url, tel, color, date, time, datetime-local, month, week, number,
    //   hidden -> text (the date family shows its format as placeholder unless one is set;
    //   color starts at #000000 like HTML);
    // - number: typed characters filtered (digits, sign, decimal point, exponent), any value
    //   sanitized to a number prefix, Up/Down step by `step` from `min` within `min`/`max`,
    //   the value clamped when the field loses focus or Enter is pressed;
    // - reset -> a button that resets its form; image -> submit drawing `src` (its intrinsic
    //   size, width/height attributes); file -> a disabled "Choose File" button;
    // - text, password, checkbox, radio, range, submit, button as RmlUi has them; other types
    //   -> text.
    // Also: form="id" for submit/image, implicit submission (Enter in a text field clicks the
    // form's first submit button, or submits a form with one text field), Space toggles
    // checkboxes and radios and activates buttons, Enter activates buttons, autofocus.
    class JASPERENGINE_INTERNAL HtmlInputElement : public Rml::ElementFormControlInput {
    public:
        RMLUI_RTTI_DeclareWithParent(HtmlInputElement, Rml::ElementFormControlInput)

        explicit HtmlInputElement(const Rml::String& tag);
        ~HtmlInputElement() override;

        // the RmlUi type for an HTML type (lowercase)
        static std::string mapType(std::string_view htmlType);
        // the format shown as placeholder for the date family ("" for other types)
        static std::string datePlaceholder(std::string_view htmlType);
        // the longest number-shaped text made of the characters of `text`, in order:
        // [sign] digits [. digits] [e [sign] digits]
        static std::string filterNumber(std::string_view text);

        const std::string& getHtmlType() const { return htmlType; }

        // form reset: the default value and checked state (the attributes the element had
        // before its first update)
        void resetToDefault();
        const std::string& getDefaultValue() const { return defaultValue; }
        bool getDefaultChecked() const { return defaultChecked; }

        // number inputs: the value moved by `steps` steps (snapped to the step grid from min,
        // within min/max); false for other types
        bool stepBy(int steps);
        // number inputs: the value clamped to min/max (false when nothing changed)
        bool clampToRange();

    protected:
        void OnAttributeChange(const Rml::ElementAttributes& changed_attributes) override;
        void OnUpdate() override;
        void OnRender() override;
        void OnResize() override;
        bool GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio) override;
        float GetBaseline() const override;
        void ProcessDefaultAction(Rml::Event& event) override;

    private:
        class Behavior;
        friend class Behavior;

        void applyType();
        void sanitizeNumber();
        bool implicitSubmit();

        std::unique_ptr<Behavior> behavior;
        std::string htmlType = "text";
        std::string defaultValue;
        bool defaultChecked = false;
        bool updated = false;
        bool sanitizing = false;

        // type=image
        decorators::Image image;
        bool imageLoaded = false;
        bool imageDirty = true;
        Rml::ReplacedContentGeometry imageGeometry;
        Rml::Texture imageTexture;
    };

}

#endif //COMPAT_RUNTIME_HTMLINPUTELEMENT_H
