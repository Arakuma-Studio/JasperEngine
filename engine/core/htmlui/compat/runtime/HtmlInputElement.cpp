// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlInputElement.h"

#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/runtime/HtmlButtonElement.h"
#include "htmlui/compat/runtime/HtmlFormSupport.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/FontMetrics.h>
#include <RmlUi/Core/Elements/ElementForm.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace jasperengine::htmlui::compat;

namespace {

    bool isTextLike(const std::string& type){
        return HtmlInputElement::mapType(type) == "text" || type == "password";
    }

    bool parseDouble(const std::string& text, double& value){
        if (text.empty())
            return false;
        char* end = nullptr;
        const double parsed = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size() || !std::isfinite(parsed))
            return false;
        value = parsed;
        return true;
    }

    std::string formatValue(double value){
        // 1.0 -> "1", 0.1 + 0.2 -> "0.3"
        return formatNumber(value, 6);
    }

}

// The listeners of an input: at the target RmlUi runs the non-capture listeners first, in the
// order they were added, so these (added by the constructor) see key, text and change events
// before RmlUi's own text widget.
class HtmlInputElement::Behavior : public Rml::EventListener {
public:
    explicit Behavior(HtmlInputElement* input): input(input) {}

    void ProcessEvent(Rml::Event& event) override{
        if (event.GetTargetElement() != input)
            return;
        const std::string& type = input->htmlType;
        switch (event.GetId()){
            case Rml::EventId::Textinput:
                if (type == "number"){
                    // only characters a number can have; the rest of the shape is sanitized on change
                    const std::string text = event.GetParameter<Rml::String>("text", "");
                    for (char c : text){
                        if (!std::isdigit((unsigned char)c) && c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E'){
                            event.PreventDefault();
                            break;
                        }
                    }
                }
                break;
            case Rml::EventId::Keydown:{
                const auto key = (Rml::Input::KeyIdentifier)event.GetParameter<int>("key_identifier", 0);
                if (type == "number" && (key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN)){
                    input->stepBy(key == Rml::Input::KI_UP ? 1 : -1);
                    event.PreventDefault();
                }else if ((key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER) && isTextLike(type) && !input->IsDisabled()){
                    // here, before RmlUi's text widget, which stops the key event
                    if (type == "number")
                        input->clampToRange();
                    input->implicitSubmit();
                }
                break;
            }
            case Rml::EventId::Blur:
                if (type == "number")
                    input->clampToRange();
                break;
            case Rml::EventId::Change:
                if (type == "number")
                    input->sanitizeNumber();
                break;
            default:
                break;
        }
    }

private:
    HtmlInputElement* input;
};

RMLUI_RTTI_Define(HtmlInputElement)

namespace {
    const Rml::EventId kBehaviorEvents[] = {Rml::EventId::Textinput, Rml::EventId::Keydown, Rml::EventId::Blur, Rml::EventId::Change};
}

HtmlInputElement::HtmlInputElement(const Rml::String& tag): Rml::ElementFormControlInput(tag){
    behavior = std::make_unique<Behavior>(this);
    for (Rml::EventId id : kBehaviorEvents)
        AddEventListener(id, behavior.get(), false);
}

HtmlInputElement::~HtmlInputElement(){
    for (Rml::EventId id : kBehaviorEvents)
        RemoveEventListener(id, behavior.get(), false);
}

std::string HtmlInputElement::mapType(std::string_view htmlType){
    static const char* const native[] = {"text", "password", "checkbox", "radio", "range", "submit", "button"};
    for (const char* type : native){
        if (htmlType == type)
            return type;
    }
    if (htmlType == "reset" || htmlType == "file")
        return "button";
    if (htmlType == "image")
        return "submit";
    return "text";
}

std::string HtmlInputElement::datePlaceholder(std::string_view htmlType){
    if (htmlType == "date")
        return "yyyy-mm-dd";
    if (htmlType == "time")
        return "hh:mm";
    if (htmlType == "datetime-local")
        return "yyyy-mm-ddThh:mm";
    if (htmlType == "month")
        return "yyyy-mm";
    if (htmlType == "week")
        return "yyyy-Www";
    return std::string();
}

std::string HtmlInputElement::filterNumber(std::string_view text){
    enum State { Sign, Integer, Fraction, ExponentSign, Exponent };
    State state = Sign;
    std::string out;
    bool digits = false;
    for (char c : text){
        const bool digit = std::isdigit((unsigned char)c) != 0;
        switch (state){
            case Sign:
                if (c == '-' || c == '+'){
                    out += c;
                    state = Integer;
                    continue;
                }
                state = Integer;
                [[fallthrough]];
            case Integer:
                if (digit){
                    out += c;
                    digits = true;
                }else if (c == '.'){
                    out += c;
                    state = Fraction;
                }else if ((c == 'e' || c == 'E') && digits){
                    out += c;
                    state = ExponentSign;
                }
                break;
            case Fraction:
                if (digit){
                    out += c;
                    digits = true;
                }else if ((c == 'e' || c == 'E') && digits){
                    out += c;
                    state = ExponentSign;
                }
                break;
            case ExponentSign:
                if (c == '-' || c == '+'){
                    out += c;
                    state = Exponent;
                }else if (digit){
                    out += c;
                    state = Exponent;
                }
                break;
            case Exponent:
                if (digit)
                    out += c;
                break;
        }
    }
    return out;
}

void HtmlInputElement::applyType(){
    if (htmlType == "file"){
        // a disabled "Choose File" button: there is no file picker
        if (!HasAttribute("disabled"))
            SetAttribute("disabled", "");
        if (GetAttribute<Rml::String>("value", "") != "Choose File")
            SetAttribute("value", "Choose File");
    }
    const std::string placeholder = datePlaceholder(htmlType);
    if (!placeholder.empty() && !HasAttribute("placeholder"))
        SetAttribute("placeholder", placeholder);
    if (htmlType == "color" && GetAttribute<Rml::String>("value", "").empty())
        SetAttribute("value", "#000000");
    imageDirty = true;
}

void HtmlInputElement::OnAttributeChange(const Rml::ElementAttributes& changed_attributes){
    // RmlUi sees the mapped type; the attribute keeps the HTML one
    Rml::ElementAttributes attributes = changed_attributes;
    const auto type = changed_attributes.find("type");
    bool typeChanged = false;
    if (type != changed_attributes.end()){
        std::string html = type->second.Get<Rml::String>("text");
        std::transform(html.begin(), html.end(), html.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        typeChanged = html != htmlType;
        htmlType = html.empty() ? "text" : html;
        attributes["type"] = Rml::Variant(mapType(htmlType));
    }
    Rml::ElementFormControlInput::OnAttributeChange(attributes);

    // defaults: the attributes before the first update (RmlUi writes the current value into
    // the value attribute later on)
    if (!updated){
        if (changed_attributes.count("value"))
            defaultValue = GetAttribute<Rml::String>("value", "");
        if (changed_attributes.count("checked"))
            defaultChecked = HasAttribute("checked");
    }
    if (typeChanged)
        applyType();
    if (changed_attributes.count("src"))
        imageDirty = true;
}

void HtmlInputElement::OnUpdate(){
    Rml::ElementFormControlInput::OnUpdate();
    if (updated)
        return;
    updated = true;
    if (htmlType == "reset")
        forms::rememberDefaults(forms::formOf(this));
    if (HasAttribute("autofocus") && !IsDisabled())
        Focus(true);
}

void HtmlInputElement::resetToDefault(){
    if (htmlType == "checkbox" || htmlType == "radio"){
        if (defaultChecked)
            SetAttribute("checked", "");
        else
            RemoveAttribute("checked");
        return;
    }
    if (mapType(htmlType) == "text" || htmlType == "range")
        SetValue(defaultValue);
}

bool HtmlInputElement::stepBy(int steps){
    if (htmlType != "number" || IsDisabled() || HasAttribute("readonly"))
        return false;
    double step = 1.0, minimum = 0.0, maximum = 0.0, value = 0.0;
    if (!parseDouble(GetAttribute<Rml::String>("step", "1"), step) || !(step > 0.0))
        step = 1.0;
    const bool hasMin = parseDouble(GetAttribute<Rml::String>("min", ""), minimum);
    const bool hasMax = parseDouble(GetAttribute<Rml::String>("max", ""), maximum);
    const double base = hasMin ? minimum : 0.0;
    if (!parseDouble(GetValue(), value))
        value = hasMin ? minimum : 0.0;
    else
        value += steps * step;
    // the step grid from min (HTML's step base)
    value = base + std::round((value - base) / step) * step;
    if (hasMin && value < minimum)
        value = minimum;
    if (hasMax && value > maximum)
        value = hasMin ? base + std::floor((maximum - base) / step) * step : maximum;
    const std::string text = formatValue(value);
    if (text == GetValue())
        return false;
    SetValue(text);
    return true;
}

bool HtmlInputElement::clampToRange(){
    if (htmlType != "number")
        return false;
    double value = 0.0, minimum = 0.0, maximum = 0.0;
    if (!parseDouble(GetValue(), value))
        return false;
    double clamped = value;
    if (parseDouble(GetAttribute<Rml::String>("min", ""), minimum) && clamped < minimum)
        clamped = minimum;
    if (parseDouble(GetAttribute<Rml::String>("max", ""), maximum) && clamped > maximum)
        clamped = maximum;
    if (clamped == value)
        return false;
    SetValue(formatValue(clamped));
    return true;
}

void HtmlInputElement::sanitizeNumber(){
    if (sanitizing)
        return;
    const std::string value = GetValue();
    const std::string filtered = filterNumber(value);
    if (filtered == value)
        return;
    sanitizing = true;
    SetValue(filtered);
    sanitizing = false;
}

bool HtmlInputElement::implicitSubmit(){
    // HTML implicit submission: Enter in a field clicks the form's first submit button; a form
    // without one is submitted when this is its only text field
    Rml::ElementForm* form = forms::formOf(this);
    if (!form)
        return false;
    Rml::ElementList candidates;
    Rml::ElementUtilities::GetElementsByTagName(candidates, form, "button");
    Rml::ElementList inputs;
    Rml::ElementUtilities::GetElementsByTagName(inputs, form, "input");
    Rml::Element* submitter = nullptr;
    for (Rml::Element* element : candidates){
        auto* button = rmlui_dynamic_cast<HtmlButtonElement*>(element);
        if (button && button->getType() == "submit"){
            submitter = element;
            break;
        }
    }
    size_t textFields = 0;
    for (Rml::Element* element : inputs){
        auto* input = rmlui_dynamic_cast<HtmlInputElement*>(element);
        if (!input)
            continue;
        if (!submitter && (input->htmlType == "submit" || input->htmlType == "image"))
            submitter = element;
        if (isTextLike(input->htmlType) && input->htmlType != "hidden")
            textFields++;
    }
    if (submitter){
        if (!forms::isDisabled(submitter))
            submitter->Click();
        return true;
    }
    if (textFields == 1){
        form->Submit();
        return true;
    }
    return false;
}

void HtmlInputElement::ProcessDefaultAction(Rml::Event& event){
    if (!IsDisabled() && event.GetTargetElement() == this){
        const std::string& type = htmlType;
        if (event == Rml::EventId::Click){
            if (type == "reset"){
                Rml::ElementFormControlInput::ProcessDefaultAction(event);
                forms::reset(forms::formOf(this));
                return;
            }
            // RmlUi's submit only looks for an ancestor form
            if ((type == "submit" || type == "image") && HasAttribute("form")){
                forms::submit(this);
                return;
            }
        }else if (event == Rml::EventId::Keydown || event == Rml::EventId::Keyup){
            const bool buttonLike = type == "submit" || type == "button" || type == "reset" || type == "image";
            if (buttonLike && forms::activateFromKey(this, event))
                return;
            if ((type == "checkbox" || type == "radio") && forms::activateFromKey(this, event, forms::Activation::SpaceOnly))
                return;
        }
    }
    Rml::ElementFormControlInput::ProcessDefaultAction(event);
}

// ---------------------------------------------------------------- type=image

// A text field or a button aligns the baseline of its text (CSS: the baseline of its line box),
// RmlUi's replaced elements their bottom margin edge (what CSS does for checkboxes, radios,
// ranges and images, kept for them). Distance from the bottom margin edge.
float HtmlInputElement::GetBaseline() const{
    if (htmlType == "checkbox" || htmlType == "radio" || htmlType == "range" || htmlType == "image" || htmlType == "hidden")
        return Rml::ElementFormControlInput::GetBaseline();
    const Rml::FontFaceHandle font = const_cast<HtmlInputElement*>(this)->GetFontFaceHandle();
    if (!font)
        return Rml::ElementFormControlInput::GetBaseline();
    const Rml::FontMetrics& metrics = Rml::GetFontEngineInterface()->GetFontMetrics(font);
    const Rml::Box& box = const_cast<HtmlInputElement*>(this)->GetBox();
    const float content = box.GetSize(Rml::BoxArea::Content).y;
    // the line is centered in the content box, the text centered in the line (half-leading)
    const float line = const_cast<HtmlInputElement*>(this)->GetLineHeight();
    const float lineTop = (content - line) * 0.5f;
    const float baselineFromTop = lineTop + (line - (metrics.ascent + metrics.descent)) * 0.5f + metrics.ascent;
    return box.GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Bottom) + box.GetEdge(Rml::BoxArea::Border, Rml::BoxEdge::Bottom) +
           box.GetEdge(Rml::BoxArea::Padding, Rml::BoxEdge::Bottom) + (content - baselineFromTop);
}

bool HtmlInputElement::GetIntrinsicDimensions(Rml::Vector2f& dimensions, float& ratio){
    if (htmlType == "range"){
        // RmlUi's slider is 16px tall; a range with an auto height (appearance: none, through the
        // user-agent sheet's companions) is as tall as its track's border box, as in Chromium
        const bool result = Rml::ElementFormControlInput::GetIntrinsicDimensions(dimensions, ratio);
        for (int i = 0; i < GetNumChildren(true); i++){
            Rml::Element* child = GetChild(i);
            if (!child || child->GetTagName() != "slidertrack")
                continue;
            const Rml::ComputedValues& computed = child->GetComputedValues();
            if (computed.height().type == Rml::Style::Height::Length)
                dimensions.y = computed.height().value + computed.padding_top().value + computed.padding_bottom().value +
                               computed.border_top_width() + computed.border_bottom_width();
            break;
        }
        return result;
    }
    if (htmlType != "image")
        return Rml::ElementFormControlInput::GetIntrinsicDimensions(dimensions, ratio);
    if (imageDirty){
        imageDirty = false;
        imageGeometry = Rml::Geometry();
        const std::string source = GetAttribute<Rml::String>("src", "");
        imageLoaded = !source.empty() && image.load(this, source);
    }
    dimensions = imageLoaded ? image.intrinsicSize() : Rml::Vector2f(0, 0);
    const float width = GetAttribute<float>("width", -1.0f), height = GetAttribute<float>("height", -1.0f);
    if (dimensions.y > 0.0f)
        ratio = dimensions.x / dimensions.y;
    if (width > 0.0f && height > 0.0f){
        dimensions = Rml::Vector2f(width, height);
        ratio = width / height;
    }else if (width > 0.0f && dimensions.y > 0.0f){
        dimensions = Rml::Vector2f(width, width / ratio);
    }else if (height > 0.0f && dimensions.y > 0.0f){
        dimensions = Rml::Vector2f(height * ratio, height);
    }
    return true;
}

void HtmlInputElement::OnResize(){
    Rml::ElementFormControlInput::OnResize();
    imageGeometry = Rml::Geometry();
}

void HtmlInputElement::OnRender(){
    Rml::ElementFormControlInput::OnRender();
    if (htmlType != "image" || !imageLoaded)
        return;
    Rml::RenderManager* renderManager = GetRenderManager();
    const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (!renderManager || size.x <= 0.0f || size.y <= 0.0f)
        return;
    if (!imageGeometry){
        imageTexture = image.texture(this, size);
        const Rml::ComputedValues& computed = GetComputedValues();
        Rml::Mesh mesh;
        Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(0, 0), size, computed.image_color().ToPremultiplied(computed.opacity()),
                                         Rml::Vector2f(0, 0), Rml::Vector2f(1, 1));
        imageGeometry = renderManager->MakeGeometry(std::move(mesh));
    }
    imageGeometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content), imageTexture);
}
