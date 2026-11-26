// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Element targets of the anime API on RmlUi. Ports Anime.js 4.5.0 (MIT) getTweenType,
// sanitizePropertyName, getOriginalAnimatableValue, parseInlineTransforms /
// buildTransformString (as Rml::TransformPrimitives), convertValueUnit and revertValues for
// HTML UI elements.

#include "AnimProperty.h"

#include "AnimCommon.h"
#include "AnimSvg.h"
#include "object/ui/UIElementInternal.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/Transform.h>
#include <RmlUi/Core/TransformPrimitive.h>

#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

using namespace jasperengine;

namespace {

    const double PI = 3.14159265358979323846;

    // Anime's validTransforms, in its order
    const std::array<const char*, 15> validTransforms = {
        "perspective", "translateX", "translateY", "translateZ", "rotate", "rotateX", "rotateY", "rotateZ",
        "scale", "scaleX", "scaleY", "scaleZ", "skew", "skewX", "skewY",
    };

    bool isTransformName(const std::string& name){
        for (const char* t : validTransforms){
            if (name == t)
                return true;
        }
        return name == "x" || name == "y" || name == "z";
    }

    const char* shortTransform(const std::string& name){
        if (name == "x") return "translateX";
        if (name == "y") return "translateY";
        if (name == "z") return "translateZ";
        return nullptr;
    }

    bool startsWith(const std::string& text, const char* prefix){
        return text.rfind(prefix, 0) == 0;
    }

    // element properties animated like an object's (Anime: `prop in target`)
    bool isElementObjectProperty(const std::string& name){
        return name == "scrollTop" || name == "scrollLeft" || name == "textContent" || name == "innerHTML";
    }

    struct UnitName { const char* name; Rml::Unit unit; };
    const UnitName unitNames[] = {
        {"px", Rml::Unit::PX}, {"%", Rml::Unit::PERCENT}, {"em", Rml::Unit::EM}, {"rem", Rml::Unit::REM},
        {"vw", Rml::Unit::VW}, {"vh", Rml::Unit::VH}, {"dp", Rml::Unit::DP}, {"in", Rml::Unit::INCH},
        {"cm", Rml::Unit::CM}, {"mm", Rml::Unit::MM}, {"pt", Rml::Unit::PT}, {"pc", Rml::Unit::PC},
        {"deg", Rml::Unit::DEG}, {"rad", Rml::Unit::RAD}, {"x", Rml::Unit::X},
    };

    bool toRmlUnit(const std::string& unit, Rml::Unit& out){
        for (const UnitName& u : unitNames){
            if (unit == u.name){
                out = u.unit;
                return true;
            }
        }
        return false;
    }

    const char* unitSuffix(Rml::Unit unit){
        for (const UnitName& u : unitNames){
            if (u.unit == unit)
                return u.name;
        }
        return "";
    }

    // a property as CSS text Anime can decompose: numbers with their unit, colours as rgba()
    std::string propertyText(const Rml::Property& property){
        if (property.unit == Rml::Unit::COLOUR){
            Rml::Colourb c = property.Get<Rml::Colourb>();
            return "rgba(" + std::to_string((int)c.red) + ", " + std::to_string((int)c.green) + ", " + std::to_string((int)c.blue) +
                   ", " + AnimMath::toString(AnimMath::round(c.alpha / 255.0, 3)) + ")";
        }
        if (Rml::Any(property.unit & Rml::Unit::NUMERIC)){
            const float number = property.Get<float>();
            return AnimMath::toString(AnimMath::round(number, 6)) + unitSuffix(property.unit);
        }
        return property.ToString();
    }

    // typed fast paths of the render (phase-1 §3.11): single lengths and colours
    bool isLengthProperty(Rml::PropertyId id){
        using Rml::PropertyId;
        switch (id){
            case PropertyId::Width: case PropertyId::Height: case PropertyId::MinWidth: case PropertyId::MinHeight:
            case PropertyId::MaxWidth: case PropertyId::MaxHeight:
            case PropertyId::Left: case PropertyId::Top: case PropertyId::Right: case PropertyId::Bottom:
            case PropertyId::MarginTop: case PropertyId::MarginRight: case PropertyId::MarginBottom: case PropertyId::MarginLeft:
            case PropertyId::PaddingTop: case PropertyId::PaddingRight: case PropertyId::PaddingBottom: case PropertyId::PaddingLeft:
            case PropertyId::BorderTopWidth: case PropertyId::BorderRightWidth: case PropertyId::BorderBottomWidth: case PropertyId::BorderLeftWidth:
            case PropertyId::RmlUi_BorderTopLeftRadiusX: case PropertyId::RmlUi_BorderTopLeftRadiusY:
            case PropertyId::RmlUi_BorderTopRightRadiusX: case PropertyId::RmlUi_BorderTopRightRadiusY:
            case PropertyId::RmlUi_BorderBottomRightRadiusX: case PropertyId::RmlUi_BorderBottomRightRadiusY:
            case PropertyId::RmlUi_BorderBottomLeftRadiusX: case PropertyId::RmlUi_BorderBottomLeftRadiusY:
            case PropertyId::FontSize: case PropertyId::LetterSpacing: case PropertyId::RowGap: case PropertyId::ColumnGap:
                return true;
            default:
                return false;
        }
    }

    bool isColorProperty(Rml::PropertyId id){
        using Rml::PropertyId;
        switch (id){
            case PropertyId::Color: case PropertyId::BackgroundColor: case PropertyId::ImageColor:
            case PropertyId::BorderTopColor: case PropertyId::BorderRightColor: case PropertyId::BorderBottomColor: case PropertyId::BorderLeftColor:
                return true;
            default:
                return false;
        }
    }

    // the element's inline (style attribute / script) value: GetLocalProperty includes the
    // matching stylesheet rules too, and Anime reads `element.style` only
    const Rml::Property* inlineProperty(Rml::Element* e, const std::string& name){
        const Rml::PropertyId id = Rml::StyleSheetSpecification::GetPropertyId(name);
        if (id == Rml::PropertyId::Invalid)
            return startsWith(name, "--") ? e->GetLocalProperty(name) : nullptr;
        const Rml::PropertyMap& properties = e->GetLocalStyleProperties();
        auto it = properties.find(id);
        return it != properties.end() ? &it->second : nullptr;
    }

    // the widths/heights Anime reads as used values (getComputedStyle), not "auto"
    bool usesBoxWidth(Rml::PropertyId id){ return id == Rml::PropertyId::Width; }
    bool usesBoxHeight(Rml::PropertyId id){ return id == Rml::PropertyId::Height; }

    class ElementTarget;

    struct Registry {
        std::unordered_map<Rml::Element*, std::weak_ptr<ElementTarget>> targets;
        std::vector<std::weak_ptr<ElementTarget>> dirty;
    };

    Registry& registry(){
        // leaked: element targets can outlive static destruction (Lua handles, the engine)
        static Registry* r = new Registry();
        return *r;
    }

    class ElementTarget : public AnimTarget {
    public:
        explicit ElementTarget(Rml::Element* e): element(e->GetObserverPtr()), key(e) {}

        ~ElementTarget() override{
            Registry& r = registry();
            auto it = r.targets.find(key);
            if (it != r.targets.end() && it->second.expired())
                r.targets.erase(it);
        }

        Kind getKind() const override{ return Kind::ELEMENT; }
        bool isValid() const override{ return element.get() != nullptr; }
        const void* getIdentity() const override{ return key; }
        Rml::Element* get() const{ return element.get(); }

        AnimTweenType getTweenType(const std::string& property) const override{
            Rml::Element* current = element.get();
            const bool cssReserved = property == "opacity" || property == "rotate" || property == "overflow" || property == "color";
            if (current && !cssReserved && current->HasAttribute(property)){
                for (Rml::Element* ancestor = current; ancestor; ancestor = ancestor->GetParentNode()){
                    if (ancestor->GetTagName() == "svg")
                        return AnimTweenType::ATTRIBUTE;
                }
            }
            if (isTransformName(property))
                return AnimTweenType::TRANSFORM;
            if (startsWith(property, "--"))
                return AnimTweenType::CSS_VAR;
            const std::string css = AnimValue::toKebabCase(property);
            if (Rml::StyleSheetSpecification::GetPropertyId(css) != Rml::PropertyId::Invalid ||
                Rml::StyleSheetSpecification::GetShorthandId(css) != Rml::ShorthandId::Invalid)
                return AnimTweenType::CSS;
            if (isElementObjectProperty(property))
                return AnimTweenType::OBJECT;
            return AnimTweenType::ATTRIBUTE;
        }

        std::string sanitizeProperty(const std::string& property, AnimTweenType type) const override{
            if (type == AnimTweenType::TRANSFORM){
                const char* t = shortTransform(property);
                return t ? t : property;
            }
            if (type == AnimTweenType::CSS)
                return AnimValue::toKebabCase(property);
            return property;
        }

        AnimRawValue getValue(const std::string& property, AnimTweenType type, AnimRawValue* inlineValue) override{
            Rml::Element* e = element.get();
            if (!e)
                return AnimRawValue(0.0);
            switch (type){
                case AnimTweenType::OBJECT: {
                    AnimRawValue value = getObjectProperty(e, property);
                    if (value.isTruthy() && inlineValue)
                        *inlineValue = value;
                    return value.isTruthy() ? value : AnimRawValue(0.0);
                }
                case AnimTweenType::ATTRIBUTE: {
                    if (!e->HasAttribute(property))
                        return AnimRawValue();
                    AnimRawValue value(e->GetAttribute<Rml::String>(property, ""));
                    if (value.isTruthy() && inlineValue)
                        *inlineValue = value;
                    return value;
                }
                case AnimTweenType::TRANSFORM:
                    return getTransformValue(e, property, inlineValue);
                case AnimTweenType::CSS_VAR: {
                    AnimRawValue value = getCssValue(e, property, inlineValue);
                    std::string text = value.toString();
                    size_t begin = text.find_first_not_of(" \t\r\n");
                    return AnimRawValue(begin == std::string::npos ? std::string() : text.substr(begin));
                }
                case AnimTweenType::CSS:
                    return getCssValue(e, property, inlineValue);
            }
            return AnimRawValue();
        }

        void setValue(const std::string& property, AnimTweenType type, const AnimWrite& value) override{
            Rml::Element* e = element.get();
            if (!e)
                return;
            switch (type){
                case AnimTweenType::OBJECT:
                    setObjectProperty(e, property, value);
                    break;
                case AnimTweenType::ATTRIBUTE:
                    if (property == "draw" && e->HasAttribute("data-anime-drawable")){
                        AnimSvg::setDraw(UIElementAccess::wrap(e), writeText(value));
                        break;
                    }
                    e->SetAttribute(property, writeText(value));
                    break;
                case AnimTweenType::TRANSFORM:
                    transforms[property] = writeText(value);
                    markDirty();
                    break;
                case AnimTweenType::CSS_VAR:
                    e->SetProperty(property, writeText(value));
                    break;
                case AnimTweenType::CSS:
                    setCssProperty(e, property, value);
                    break;
            }
        }

        void revertValue(const std::string& property, AnimTweenType type, const AnimRawValue& original) override{
            Rml::Element* e = element.get();
            if (!e)
                return;
            const bool hadNoInlineValue = original.isNil() || (original.isString() && original.text.empty());
            switch (type){
                case AnimTweenType::OBJECT:
                    if (!hadNoInlineValue){
                        AnimWrite w;
                        w.valueType = original.isNumber() ? AnimValueType::NUMBER : AnimValueType::COMPLEX;
                        w.number = original.isNumber() ? original.number : 0.0;
                        w.text = original.isNumber() ? std::string() : original.text;
                        setObjectProperty(e, property, w);
                    }
                    break;
                case AnimTweenType::ATTRIBUTE:
                    if (hadNoInlineValue){
                        e->RemoveAttribute(property);
                    } else if (property == "draw" && e->HasAttribute("data-anime-drawable")){
                        AnimSvg::setDraw(UIElementAccess::wrap(e), original.toString());
                    } else {
                        e->SetAttribute(property, original.toString());
                    }
                    break;
                case AnimTweenType::TRANSFORM:
                    if (hadNoInlineValue){
                        transforms.erase(property);
                    } else {
                        transforms[property] = original.toString();
                    }
                    if (transforms.empty()){
                        e->RemoveProperty(Rml::PropertyId::Transform);
                        dirty = false;
                    } else {
                        markDirty();
                    }
                    break;
                case AnimTweenType::CSS_VAR:
                case AnimTweenType::CSS:
                    if (hadNoInlineValue){
                        e->RemoveProperty(property);
                    } else {
                        e->SetProperty(property, original.toString());
                    }
                    break;
            }
        }

        void convertUnit(AnimDecomposed& value, const std::string& unit, bool force) override{
            (void)force;
            if (value.t == AnimValueType::UNIT && value.u == unit)
                return;
            Rml::Element* e = element.get();
            static const std::unordered_map<std::string, double> angles = {{"deg", 1}, {"rad", 180 / PI}, {"turn", 360}, {"grad", 0.9}};
            auto from = angles.find(value.u);
            auto to = angles.find(unit);
            if (from != angles.end() && to != angles.end()){
                value.n = value.n * from->second / to->second;
            } else if (e){
                // lengths through px: Anime measures a 100<unit> wide clone, here RmlUi resolves
                const double fromPx = toPx(e, value.n, value.u);
                const double unitPx = toPx(e, 100, unit) / 100;
                if (!std::isnan(fromPx) && !std::isnan(unitPx) && unitPx != 0)
                    value.n = fromPx / unitPx;
            }
            value.u = unit;
        }

        AnimRawValue resolveCssVariable(const std::string& name) override{
            Rml::Element* e = element.get();
            if (!e)
                return AnimRawValue();
            const Rml::Property* p = e->GetProperty(name);
            return p ? AnimRawValue(propertyText(*p)) : AnimRawValue();
        }

        bool getStaggerPosition(double& x, double& y, double& z, bool& has3D) override{
            Rml::Element* e = element.get();
            if (!e)
                return false;
            Rml::Vector2f offset = e->GetAbsoluteOffset(Rml::BoxArea::Border);
            Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Border);
            x = offset.x + size.x / 2;
            y = offset.y + size.y / 2;
            z = 0;
            has3D = false;
            return true;
        }

        // the transform written at the end of the render pass
        void flushTransforms(){
            dirty = false;
            Rml::Element* e = element.get();
            if (!e)
                return;
            Rml::TransformPtr transform = Rml::MakeShared<Rml::Transform>();
            for (const char* name : validTransforms){
                auto it = transforms.find(name);
                if (it != transforms.end())
                    addPrimitive(*transform, name, it->second);
                // non-animatable rotate3d keeps its place after rotateZ
                if (std::string(name) == "rotateZ"){
                    auto r3 = transforms.find("rotate3d");
                    if (r3 != transforms.end())
                        addPrimitive(*transform, "rotate3d", r3->second);
                }
            }
            auto matrix = transforms.find("matrix");
            if (matrix != transforms.end())
                addPrimitive(*transform, "matrix", matrix->second);
            auto matrix3d = transforms.find("matrix3d");
            if (matrix3d != transforms.end())
                addPrimitive(*transform, "matrix3d", matrix3d->second);

            Rml::Property property(transform, Rml::Unit::TRANSFORM);
            property.definition = Rml::StyleSheetSpecification::GetProperty(Rml::PropertyId::Transform);
            e->SetProperty(Rml::PropertyId::Transform, property);
        }

    private:
        void markDirty(){
            if (dirty)
                return;
            dirty = true;
            registry().dirty.push_back(std::static_pointer_cast<ElementTarget>(shared_from_this()));
        }

        static std::string writeText(const AnimWrite& value){
            return value.valueType == AnimValueType::NUMBER ? AnimMath::toString(value.number) : value.text;
        }

        static AnimRawValue getObjectProperty(Rml::Element* e, const std::string& property){
            if (property == "scrollTop") return AnimRawValue((double)e->GetScrollTop());
            if (property == "scrollLeft") return AnimRawValue((double)e->GetScrollLeft());
            UIElement wrapped = UIElementAccess::wrap(e);
            if (property == "textContent") return AnimRawValue(wrapped.getTextContent());
            if (property == "innerHTML") return AnimRawValue(wrapped.getInnerHTML());
            return AnimRawValue();
        }

        static void setObjectProperty(Rml::Element* e, const std::string& property, const AnimWrite& value){
            const std::string text = writeText(value);
            if (property == "scrollTop"){
                e->SetScrollTop((float)AnimMath::toNumber(text));
            } else if (property == "scrollLeft"){
                e->SetScrollLeft((float)AnimMath::toNumber(text));
            } else {
                UIElement wrapped = UIElementAccess::wrap(e);
                if (property == "textContent") wrapped.setTextContent(text);
                if (property == "innerHTML") wrapped.setInnerHTML(text);
            }
        }

        static AnimRawValue getCssValue(Rml::Element* e, const std::string& name, AnimRawValue* inlineValue){
            // a corner radius is a shorthand of its horizontal and vertical radius in the fork
            // (phase 3, p3-paint): its value reads as the horizontal one
            const bool cornerRadius = startsWith(name, "border-") && name.size() > 14 &&
                name.compare(name.size() - 7, 7, "-radius") == 0 &&
                Rml::StyleSheetSpecification::GetPropertyId(name) == Rml::PropertyId::Invalid &&
                Rml::StyleSheetSpecification::GetPropertyId("-rmlui-" + name + "-x") != Rml::PropertyId::Invalid;
            const std::string property = cornerRadius ? "-rmlui-" + name + "-x" : name;
            const Rml::Property* local = inlineProperty(e, property);
            std::string inlineText = local ? propertyText(*local) : std::string();
            if (!inlineText.empty() && inlineValue)
                *inlineValue = AnimRawValue(inlineText);
            std::string value = inlineText;
            if (value.empty()){
                const Rml::Property* computed = e->GetProperty(property);
                if (computed){
                    const Rml::PropertyId id = Rml::StyleSheetSpecification::GetPropertyId(property);
                    if (computed->unit == Rml::Unit::KEYWORD && (usesBoxWidth(id) || usesBoxHeight(id))){
                        // used size, as getComputedStyle reports it
                        Rml::Vector2f size = e->GetBox().GetSize(Rml::BoxArea::Content);
                        value = AnimMath::toString(AnimMath::round(usesBoxWidth(id) ? size.x : size.y, 4)) + "px";
                    } else {
                        value = propertyText(*computed);
                    }
                }
            }
            return AnimRawValue(value == "auto" ? std::string("0") : value);
        }

        static void setCssProperty(Rml::Element* e, const std::string& property, const AnimWrite& value){
            const Rml::PropertyId id = Rml::StyleSheetSpecification::GetPropertyId(property);
            if (id != Rml::PropertyId::Invalid){
                const Rml::PropertyDefinition* definition = Rml::StyleSheetSpecification::GetProperty(id);
                if (value.valueType == AnimValueType::NUMBER && id == Rml::PropertyId::Opacity){
                    Rml::Property p((float)value.number, Rml::Unit::NUMBER);
                    p.definition = definition;
                    e->SetProperty(id, p);
                    return;
                }
                Rml::Unit unit;
                if (value.valueType == AnimValueType::UNIT && value.unit && isLengthProperty(id) && toRmlUnit(*value.unit, unit) &&
                    Rml::Any(unit & Rml::Unit::LENGTH_PERCENT)){
                    Rml::Property p((float)value.number, unit);
                    p.definition = definition;
                    e->SetProperty(id, p);
                    return;
                }
                if (value.valueType == AnimValueType::COLOR && value.numbers && value.numbers->size() >= 4 && isColorProperty(id)){
                    const std::vector<double>& c = *value.numbers;
                    auto channel = [](double v){ return (Rml::byte)AnimMath::clamp(AnimMath::round(v, 0), 0, 255); };
                    Rml::Colourb colour(channel(c[0]), channel(c[1]), channel(c[2]), channel(c[3] * 255));
                    Rml::Property p(colour, Rml::Unit::COLOUR);
                    p.definition = definition;
                    e->SetProperty(id, p);
                    return;
                }
            }
            e->SetProperty(property, writeText(value));
        }

        AnimRawValue getTransformValue(Rml::Element* e, const std::string& property, AnimRawValue* inlineValue){
            const Rml::Property* local = inlineProperty(e, "transform");
            bool hasInline = false;
            if (local && local->unit == Rml::Unit::TRANSFORM){
                Rml::TransformPtr transform = local->Get<Rml::TransformPtr>();
                if (transform && !transform->GetPrimitives().empty()){
                    hasInline = true;
                    parseInlineTransform(*transform);
                }
            }
            if (hasInline){
                auto it = transforms.find(property);
                if (it != transforms.end()){
                    if (inlineValue)
                        *inlineValue = AnimRawValue(it->second);
                    return AnimRawValue(it->second);
                }
            }
            if (startsWith(property, "scale")) return AnimRawValue("1");
            if (startsWith(property, "rotate") || startsWith(property, "skew")) return AnimRawValue("0deg");
            return AnimRawValue("0px");
        }

        static std::string numeric(const Rml::NumericValue& v){
            return AnimMath::toString(AnimMath::round(v.number, 6)) + unitSuffix(v.unit);
        }

        // parseInlineTransforms: the element's transform split into the individual entries
        void parseInlineTransform(const Rml::Transform& transform){
            for (const Rml::TransformPrimitive& p : transform.GetPrimitives()){
                switch (p.type){
                    case Rml::TransformPrimitive::TRANSLATEX: transforms["translateX"] = numeric(p.translate_x.values[0]); break;
                    case Rml::TransformPrimitive::TRANSLATEY: transforms["translateY"] = numeric(p.translate_y.values[0]); break;
                    case Rml::TransformPrimitive::TRANSLATEZ: transforms["translateZ"] = numeric(p.translate_z.values[0]); break;
                    case Rml::TransformPrimitive::TRANSLATE2D:
                        transforms["translateX"] = numeric(p.translate_2d.values[0]);
                        transforms["translateY"] = numeric(p.translate_2d.values[1]);
                        break;
                    case Rml::TransformPrimitive::TRANSLATE3D:
                        transforms["translateX"] = numeric(p.translate_3d.values[0]);
                        transforms["translateY"] = numeric(p.translate_3d.values[1]);
                        transforms["translateZ"] = numeric(p.translate_3d.values[2]);
                        break;
                    case Rml::TransformPrimitive::SCALEX: transforms["scaleX"] = AnimMath::toString(p.scale_x.values[0]); break;
                    case Rml::TransformPrimitive::SCALEY: transforms["scaleY"] = AnimMath::toString(p.scale_y.values[0]); break;
                    case Rml::TransformPrimitive::SCALEZ: transforms["scaleZ"] = AnimMath::toString(p.scale_z.values[0]); break;
                    case Rml::TransformPrimitive::SCALE2D:
                        if (p.scale_2d.values[0] == p.scale_2d.values[1]){
                            transforms["scale"] = AnimMath::toString(p.scale_2d.values[0]);
                        } else {
                            transforms["scaleX"] = AnimMath::toString(p.scale_2d.values[0]);
                            transforms["scaleY"] = AnimMath::toString(p.scale_2d.values[1]);
                        }
                        break;
                    case Rml::TransformPrimitive::SCALE3D:
                        transforms["scaleX"] = AnimMath::toString(p.scale_3d.values[0]);
                        transforms["scaleY"] = AnimMath::toString(p.scale_3d.values[1]);
                        transforms["scaleZ"] = AnimMath::toString(p.scale_3d.values[2]);
                        break;
                    case Rml::TransformPrimitive::ROTATEX: transforms["rotateX"] = angle(p.rotate_x.values[0]); break;
                    case Rml::TransformPrimitive::ROTATEY: transforms["rotateY"] = angle(p.rotate_y.values[0]); break;
                    case Rml::TransformPrimitive::ROTATEZ: transforms["rotateZ"] = angle(p.rotate_z.values[0]); break;
                    case Rml::TransformPrimitive::ROTATE2D: transforms["rotate"] = angle(p.rotate_2d.values[0]); break;
                    case Rml::TransformPrimitive::ROTATE3D:
                        transforms["rotate3d"] = AnimMath::toString(p.rotate_3d.values[0]) + ", " + AnimMath::toString(p.rotate_3d.values[1]) + ", " +
                                                 AnimMath::toString(p.rotate_3d.values[2]) + ", " + angle(p.rotate_3d.values[3]);
                        break;
                    case Rml::TransformPrimitive::SKEWX: transforms["skewX"] = angle(p.skew_x.values[0]); break;
                    case Rml::TransformPrimitive::SKEWY: transforms["skewY"] = angle(p.skew_y.values[0]); break;
                    case Rml::TransformPrimitive::SKEW2D:
                        if (p.skew_2d.values[1] == 0){
                            transforms["skew"] = angle(p.skew_2d.values[0]);
                        } else {
                            transforms["skewX"] = angle(p.skew_2d.values[0]);
                            transforms["skewY"] = angle(p.skew_2d.values[1]);
                        }
                        break;
                    case Rml::TransformPrimitive::PERSPECTIVE: transforms["perspective"] = numeric(p.perspective.values[0]); break;
                    case Rml::TransformPrimitive::MATRIX2D: {
                        std::string m;
                        for (int i = 0; i < 6; i++) m += (i ? ", " : "") + AnimMath::toString(p.matrix_2d.values[i]);
                        transforms["matrix"] = m;
                        break;
                    }
                    case Rml::TransformPrimitive::MATRIX3D: {
                        std::string m;
                        for (int i = 0; i < 16; i++) m += (i ? ", " : "") + AnimMath::toString(p.matrix_3d.values[i]);
                        transforms["matrix3d"] = m;
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        // resolved angle primitives hold radians
        static std::string angle(float radians){
            return AnimMath::toString(AnimMath::round(radians * 180.0 / PI, 6)) + "deg";
        }

        static Rml::NumericValue lengthValue(const std::string& text){
            double number = 0;
            std::string unit;
            Rml::Unit u = Rml::Unit::PX;
            if (AnimValue::splitUnit(text, number, unit)){
                if (!toRmlUnit(unit, u) || !Rml::Any(u & Rml::Unit::LENGTH_PERCENT))
                    u = Rml::Unit::PX;
            } else {
                number = AnimMath::parseFloat(text);
            }
            return Rml::NumericValue(std::isnan(number) ? 0.f : (float)number, u);
        }

        static float angleDegrees(const std::string& text){
            double number = 0;
            std::string unit;
            if (AnimValue::splitUnit(text, number, unit)){
                if (unit == "rad") number = number * 180 / PI;
                else if (unit == "turn") number *= 360;
                else if (unit == "grad") number *= 0.9;
            } else {
                number = AnimMath::parseFloat(text);
            }
            return std::isnan(number) ? 0.f : (float)number;
        }

        static float plain(const std::string& text){
            double number = AnimMath::parseFloat(text);
            return std::isnan(number) ? 0.f : (float)number;
        }

        static void addPrimitive(Rml::Transform& transform, const std::string& name, const std::string& value){
            namespace T = Rml::Transforms;
            if (name == "translateX"){ Rml::NumericValue v = lengthValue(value); transform.AddPrimitive(T::TranslateX(&v)); }
            else if (name == "translateY"){ Rml::NumericValue v = lengthValue(value); transform.AddPrimitive(T::TranslateY(&v)); }
            else if (name == "translateZ"){ Rml::NumericValue v = lengthValue(value); transform.AddPrimitive(T::TranslateZ(&v)); }
            else if (name == "rotate") transform.AddPrimitive(T::Rotate2D(angleDegrees(value), Rml::Unit::DEG));
            else if (name == "rotateX") transform.AddPrimitive(T::RotateX(angleDegrees(value), Rml::Unit::DEG));
            else if (name == "rotateY") transform.AddPrimitive(T::RotateY(angleDegrees(value), Rml::Unit::DEG));
            else if (name == "rotateZ") transform.AddPrimitive(T::RotateZ(angleDegrees(value), Rml::Unit::DEG));
            else if (name == "scale") transform.AddPrimitive(T::Scale2D(plain(value)));
            else if (name == "scaleX") transform.AddPrimitive(T::ScaleX(plain(value)));
            else if (name == "scaleY") transform.AddPrimitive(T::ScaleY(plain(value)));
            else if (name == "scaleZ") transform.AddPrimitive(T::ScaleZ(plain(value)));
            else if (name == "skew") transform.AddPrimitive(T::Skew2D(angleDegrees(value), 0.f, Rml::Unit::DEG));
            else if (name == "skewX") transform.AddPrimitive(T::SkewX(angleDegrees(value), Rml::Unit::DEG));
            else if (name == "skewY") transform.AddPrimitive(T::SkewY(angleDegrees(value), Rml::Unit::DEG));
            else if (name == "perspective"){ Rml::NumericValue v = lengthValue(value); transform.AddPrimitive(T::Perspective(&v)); }
            else {
                std::vector<double> numbers;
                std::vector<std::string> strings;
                AnimValue::splitComplex(value, numbers, strings);
                if (name == "rotate3d" && numbers.size() >= 4){
                    double a = numbers[3];
                    const std::string& unit = strings.size() > 4 ? strings[4] : std::string();
                    if (unit.find("rad") != std::string::npos) a = a * 180 / PI;
                    else if (unit.find("turn") != std::string::npos) a *= 360;
                    transform.AddPrimitive(T::Rotate3D((float)numbers[0], (float)numbers[1], (float)numbers[2], (float)a, Rml::Unit::DEG));
                } else if (name == "matrix" && numbers.size() >= 6){
                    Rml::NumericValue values[6];
                    for (int i = 0; i < 6; i++) values[i] = Rml::NumericValue((float)numbers[i], Rml::Unit::NUMBER);
                    transform.AddPrimitive(T::Matrix2D(values));
                } else if (name == "matrix3d" && numbers.size() >= 16){
                    Rml::NumericValue values[16];
                    for (int i = 0; i < 16; i++) values[i] = Rml::NumericValue((float)numbers[i], Rml::Unit::NUMBER);
                    transform.AddPrimitive(T::Matrix3D(values));
                }
            }
        }

        // a length in px on this element (NaN when the unit is not a length)
        static double toPx(Rml::Element* e, double number, const std::string& unit){
            if (unit.empty() || unit == "px")
                return number;
            Rml::Unit u;
            if (!toRmlUnit(unit, u) || !Rml::Any(u & Rml::Unit::LENGTH_PERCENT))
                return std::nan("");
            if (u == Rml::Unit::PERCENT){
                Rml::Element* parent = e->GetParentNode();
                const float base = parent ? parent->GetBox().GetSize(Rml::BoxArea::Content).x : e->GetContainingBlock().x;
                return number * base / 100.0;
            }
            return e->ResolveLength(Rml::NumericValue((float)number, u));
        }

        Rml::ObserverPtr<Rml::Element> element;
        Rml::Element* key = nullptr;
        std::unordered_map<std::string, std::string> transforms;
        bool dirty = false;
    };

}

AnimTargetPtr AnimProperty::elementTarget(Rml::Element* element){
    if (!element)
        return nullptr;
    Registry& r = registry();
    auto it = r.targets.find(element);
    if (it != r.targets.end()){
        std::shared_ptr<ElementTarget> existing = it->second.lock();
        // a destroyed element's address can be reused by a new element
        if (existing && existing->get() == element)
            return existing;
    }
    std::shared_ptr<ElementTarget> target = std::make_shared<ElementTarget>(element);
    r.targets[element] = target;
    return target;
}

Rml::Element* AnimProperty::getElement(const AnimTarget& target){
    if (target.getKind() != AnimTarget::Kind::ELEMENT)
        return nullptr;
    return static_cast<const ElementTarget&>(target).get();
}

void AnimProperty::flush(){
    Registry& r = registry();
    if (r.dirty.empty())
        return;
    std::vector<std::weak_ptr<ElementTarget>> dirty;
    dirty.swap(r.dirty);
    for (const std::weak_ptr<ElementTarget>& weak : dirty){
        std::shared_ptr<ElementTarget> target = weak.lock();
        if (target)
            target->flushTransforms();
    }
}

size_t AnimProperty::getElementTargetCount(){
    size_t count = 0;
    for (const auto& entry : registry().targets){
        if (!entry.second.expired())
            count++;
    }
    return count;
}
