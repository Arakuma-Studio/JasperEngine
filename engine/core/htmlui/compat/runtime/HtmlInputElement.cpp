// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/runtime/HtmlInputElement.h"

#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/runtime/HtmlButtonElement.h"
#include "htmlui/compat/runtime/HtmlFormSupport.h"
#include "htmlui/compat/VirtualFiles.h"
#include "../../../../libs/rmlui/Source/Core/ElementBackgroundBorder.h"

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
#include <RmlUi/Core/RenderBox.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

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

bool HtmlInputElement::HasNativeAppearance() const{
    return forms::hasNativeAppearance(this);
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
    if (htmlType == "checkbox" || htmlType == "radio"){
        // Chromium's theme makes an auto width or height 13px (the border box). Without the theme
        // (`appearance: none`; the user-agent sheet's --html-check-size companion is auto) a
        // checkbox or radio is an inline-block sized by its content (its ::before and ::after,
        // padding and border; 0x0 without), so it is not replaced: laid out as a block container.
        if (!HasNativeAppearance())
            return false;
        // each auto dimension on its own: no ratio (a 20px wide checkbox stays 13px tall); the
        // themed box has no border or padding (the fork's P4-G12)
        dimensions = Rml::Vector2f(13.0f, 13.0f);
        ratio = -1.0f;
        return true;
    }
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
        imageGeometry.Clear();
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
    imageGeometry.Clear();
    themeGeometry.Clear();
}

// ---------------------------------------------------------------- themed checkbox and radio

namespace {
    // a colour of the theme (the user-agent sheet's --html-theme-* properties: #rgb[a] or
    // #rrggbb[aa]); transparent when it is none of them
    Rml::Colourb themeColour(Rml::Element* element, const char* property){
        const Rml::Property* value = element->GetProperty(property);
        std::string text = value ? Rml::StringUtilities::StripWhitespace(value->Get<Rml::String>()) : std::string();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (text.size() == 4 || text.size() == 5){
            std::string longer = "#";
            for (size_t i = 1; i < text.size(); i++)
                longer += std::string(2, text[i]);
            text = longer;
        }
        if ((text.size() != 7 && text.size() != 9) || text[0] != '#' || text.find_first_not_of("0123456789abcdef", 1) != std::string::npos)
            return Rml::Colourb(0, 0, 0, 0);
        const auto channel = [&](size_t at){ return (Rml::byte)std::strtol(text.substr(at, 2).c_str(), nullptr, 16); };
        return Rml::Colourb(channel(1), channel(3), channel(5), text.size() == 9 ? channel(7) : (Rml::byte)255);
    }

    // the colour with its HSL lightness moved by `delta` (0..1)
    Rml::Colourb shiftLightness(Rml::Colourb colour, float delta){
        const float r = colour.red / 255.0f, g = colour.green / 255.0f, b = colour.blue / 255.0f;
        const float max = std::max(r, std::max(g, b)), min = std::min(r, std::min(g, b));
        float h = 0.0f, s = 0.0f;
        const float l = (max + min) * 0.5f;
        if (max > min){
            const float d = max - min;
            s = l > 0.5f ? d / (2.0f - max - min) : d / (max + min);
            if (max == r)
                h = (g - b) / d + (g < b ? 6.0f : 0.0f);
            else if (max == g)
                h = (b - r) / d + 2.0f;
            else
                h = (r - g) / d + 4.0f;
            h /= 6.0f;
        }
        const float lightness = std::min(std::max(l + delta, 0.0f), 1.0f);
        const auto hue = [](float p, float q, float t){
            if (t < 0.0f)
                t += 1.0f;
            if (t > 1.0f)
                t -= 1.0f;
            if (t < 1.0f / 6.0f)
                return p + (q - p) * 6.0f * t;
            if (t < 0.5f)
                return q;
            if (t < 2.0f / 3.0f)
                return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
            return p;
        };
        float nr = lightness, ng = lightness, nb = lightness;
        if (s > 0.0f){
            const float q = lightness < 0.5f ? lightness * (1.0f + s) : lightness + s - lightness * s;
            const float p = 2.0f * lightness - q;
            nr = hue(p, q, h + 1.0f / 3.0f);
            ng = hue(p, q, h);
            nb = hue(p, q, h - 1.0f / 3.0f);
        }
        const auto byte = [](float v){ return (Rml::byte)std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f); };
        return Rml::Colourb(byte(nr), byte(ng), byte(nb), colour.alpha);
    }

    float relativeLuminance(Rml::Colourb colour){
        return (0.2126f * colour.red + 0.7152f * colour.green + 0.0722f * colour.blue) / 255.0f;
    }

    std::string number(float value){
        char text[32];
        std::snprintf(text, sizeof(text), "%.4g", value);
        return text;
    }

    // ` attribute="#rrggbb" attribute-opacity="a"`, or ` attribute="none"`
    std::string paintAttribute(const char* attribute, Rml::Colourb colour){
        if (colour.alpha == 0)
            return std::string(" ") + attribute + "=\"none\"";
        char text[96];
        std::snprintf(text, sizeof(text), " %s=\"#%02x%02x%02x\"", attribute, colour.red, colour.green, colour.blue);
        std::string paint = text;
        if (colour.alpha < 255){
            std::snprintf(text, sizeof(text), " %s-opacity=\"%.4f\"", attribute, colour.alpha / 255.0);
            paint += text;
        }
        return paint;
    }
}

// The theme's colours for the control's state: the user-agent sheet's --html-theme-fill, -stroke
// and -mark; hovered or pressed (its --html-theme-shade), a checked control's accent colours
// (the checkbox's fill, the radio's ring and dot) as Chromium shades them: their HSL lightness
// 11% lower when hovered and higher when pressed, the other way round for a light accent (one
// with a dark mark). The unchecked frame's hover and press colours are the sheet's.
HtmlInputElement::ThemeColours HtmlInputElement::themeColours(){
    ThemeColours colours;
    colours.fill = themeColour(this, "--html-theme-fill");
    colours.stroke = themeColour(this, "--html-theme-stroke");
    colours.mark = themeColour(this, "--html-theme-mark");
    const Rml::Property* shade = GetProperty("--html-theme-shade");
    const std::string state = shade ? Rml::StringUtilities::StripWhitespace(shade->Get<Rml::String>()) : std::string();
    if ((state == "hover" || state == "active") && IsPseudoClassSet("checked")){
        const bool radio = htmlType == "radio";
        // the mark drawn on the accent: the checkbox's check, the radio's gap
        const Rml::Colourb onAccent = radio ? colours.fill : colours.mark;
        const bool lightAccent = onAccent.alpha > 0 && relativeLuminance(onAccent) < 0.5f;
        const float delta = ((state == "hover") != lightAccent ? -1.0f : 1.0f) * 0.11f;
        if (radio){
            colours.stroke = shiftLightness(colours.stroke, delta);
            colours.mark = shiftLightness(colours.mark, delta);
        }else{
            colours.fill = shiftLightness(colours.fill, delta);
        }
    }
    return colours;
}

// Chromium's checkbox (NativeThemeBase, measured against the pinned Chromium: this SVG matches its
// pixels at 13 to 100px): a square of the box's smaller side, centered (at a fraction of a pixel
// too), a rounded square (radius 2px) filled and stroked 1px inside its edge; checked, the whole
// square filled and a check stroked 16% of its side through (20%, 50%), (40%, 70%), (80%, 20%).
// (A radio is a mesh: renderRadio.)
std::string HtmlInputElement::themeSvg(Rml::Vector2f size){
    const float side = std::min(size.x, size.y);
    const float x = (size.x - side) * 0.5f, y = (size.y - side) * 0.5f;
    const ThemeColours colours = themeColours();
    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + number(size.x) + "\" height=\"" + number(size.y) +
                      "\" viewBox=\"0 0 " + number(size.x) + " " + number(size.y) + "\">";
    if (IsPseudoClassSet("checked")){
        svg += "<rect x=\"" + number(x) + "\" y=\"" + number(y) + "\" width=\"" + number(side) + "\" height=\"" + number(side) +
               "\" rx=\"2\"" + paintAttribute("fill", colours.fill) + "/>";
        const auto point = [&](float px, float py){ return number(x + px * side) + " " + number(y + py * side); };
        svg += "<path d=\"M" + point(0.2f, 0.5f) + " L" + point(0.4f, 0.7f) + " L" + point(0.8f, 0.2f) + "\" fill=\"none\"" +
               paintAttribute("stroke", colours.mark) + " stroke-width=\"" + number(side * 0.16f) + "\"/>";
    }else{
        svg += "<rect x=\"" + number(x + 0.5f) + "\" y=\"" + number(y + 0.5f) + "\" width=\"" + number(std::max(side - 1.0f, 0.0f)) +
               "\" height=\"" + number(std::max(side - 1.0f, 0.0f)) + "\" rx=\"2\"" + paintAttribute("fill", colours.fill) +
               paintAttribute("stroke", colours.stroke) + " stroke-width=\"1\"/>";
    }
    return svg + "</svg>";
}

// Chromium's radio: a circle of the box's smaller side, centered, filled and with a 1px ring;
// checked, a dot of 60% of it. Drawn as the fork draws a `border-radius: 50%` box with a border
// and its background (the feathered edges of P3-P1/P3-P2, which match Chromium's circles; an SVG
// rasterized by LunaSVG does not).
Rml::Mesh HtmlInputElement::themeRadioMesh(Rml::Vector2f size, float featherUnit, float opacity){
    const float side = std::min(size.x, size.y);
    const Rml::Vector2f origin((size.x - side) * 0.5f, (size.y - side) * 0.5f);
    const ThemeColours colours = themeColours();
    Rml::Mesh mesh;
    if (side <= 0.0f)
        return mesh;
    const auto append = [&](Rml::Vector2f offset, float diameter, float border, Rml::Colourb background, Rml::Colourb ring){
        const size_t first = mesh.vertices.size();
        const float radius = diameter * 0.5f;
        const Rml::RenderBox box(Rml::Vector2f(std::max(diameter - 2.0f * border, 0.0f)), Rml::Vector2f(0, 0), Rml::EdgeSizes{border, border, border, border},
                                 Rml::CornerSizes{radius, radius, radius, radius});
        const Rml::ColourbPremultiplied borders[4] = {ring.ToPremultiplied(opacity), ring.ToPremultiplied(opacity), ring.ToPremultiplied(opacity),
                                                      ring.ToPremultiplied(opacity)};
        Rml::MeshUtilities::GenerateBackgroundBorder(mesh, box, background.ToPremultiplied(opacity), borders, featherUnit);
        for (size_t i = first; i < mesh.vertices.size(); i++)
            mesh.vertices[i].position += offset;
    };
    // (the ring 0.15px inside the box and 0.9px wide, the dot 0.2px smaller: the closest of the
    // measured variants to Chromium's anti-aliased circles, phase-4 spec 4.3.3 G6)
    constexpr float ringInset = 0.15f, ringWidth = 0.9f, dotInset = 0.1f;
    append(origin + Rml::Vector2f(ringInset), side - 2.0f * ringInset, ringWidth, colours.fill, colours.stroke);
    if (IsPseudoClassSet("checked")){
        const float dot = side * 0.6f - 2.0f * dotInset;
        append(origin + Rml::Vector2f((side - dot) * 0.5f), dot, 0.0f, colours.mark, Rml::Colourb(0, 0, 0, 0));
    }
    return mesh;
}

void HtmlInputElement::renderTheme(){
    Rml::RenderManager* renderManager = GetRenderManager();
    const Rml::Box& box = GetBox();
    const Rml::Vector2f size = box.GetSize(Rml::BoxArea::Content);   // the border box: a themed box has no border or padding
    if (!renderManager || size.x <= 0.0f || size.y <= 0.0f)
        return;
    const Rml::Vector2f position = GetAbsoluteOffset(Rml::BoxArea::Border) + box.GetPosition(Rml::BoxArea::Content);
    if (htmlType == "radio"){
        const float featherUnit = Rml::ElementBackgroundBorder::GetFeatherUnit(this);
        const float opacity = GetDrawOpacity();
        const ThemeColours colours = themeColours();
        char key[160];
        std::snprintf(key, sizeof(key), "%g %g %g %g %d %08x %08x %08x", size.x, size.y, featherUnit, opacity, IsPseudoClassSet("checked") ? 1 : 0,
                      (unsigned)colours.fill.red << 24 | colours.fill.green << 16 | colours.fill.blue << 8 | colours.fill.alpha,
                      (unsigned)colours.stroke.red << 24 | colours.stroke.green << 16 | colours.stroke.blue << 8 | colours.stroke.alpha,
                      (unsigned)colours.mark.red << 24 | colours.mark.green << 16 | colours.mark.blue << 8 | colours.mark.alpha);
        if (key != themeMeshKey || !themeMesh){
            themeMeshKey = key;
            themeMesh = renderManager->MakeGeometry(themeRadioMesh(size, featherUnit, opacity));
        }
        themeMesh.Render(position);
        return;
    }
    const std::string svg = themeSvg(size);
    if (svg != themeXml){
        themeXml = svg;
        // a virtual file per drawing (the shapes are few: shared by every control drawn alike)
        const std::string path = "@htmlui/theme/" + std::to_string(std::hash<std::string>()(svg)) + ".svg";
        if (!VirtualFiles::exists(path))
            VirtualFiles::add(path, svg);
        themeImage.load(this, "/" + path);
        themeGeometry.Clear();
    }
    if (!themeImage.isSvg())
        return;
    if (!themeGeometry.IsSet()){
        themeTexture = themeImage.texture(this, size);
        themeGeometry.Set(Rml::Rectanglef::FromPositionSize(box.GetPosition(Rml::BoxArea::Content), size),
                          Rml::Colourb(255, 255, 255, 255).ToPremultiplied(GetDrawOpacity()));
    }
    themeGeometry.Render(this, themeTexture);
}

void HtmlInputElement::OnRender(){
    Rml::ElementFormControlInput::OnRender();
    if ((htmlType == "checkbox" || htmlType == "radio") && HasThemedBox())
        renderTheme();
    if (htmlType != "image" || !imageLoaded)
        return;
    Rml::RenderManager* renderManager = GetRenderManager();
    const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
    if (!renderManager || size.x <= 0.0f || size.y <= 0.0f)
        return;
    if (!imageGeometry.IsSet()){
        imageTexture = image.texture(this, size);
        const Rml::ComputedValues& computed = GetComputedValues();
        imageGeometry.Set(Rml::Rectanglef::FromPositionSize(GetBox().GetPosition(Rml::BoxArea::Content), size),
                          computed.image_color().ToPremultiplied(GetDrawOpacity()));
    }
    // trimmed to the content edge curve of a rounded box, as an <img> (the fork's P4-G1)
    imageGeometry.Render(this, imageTexture);
}
