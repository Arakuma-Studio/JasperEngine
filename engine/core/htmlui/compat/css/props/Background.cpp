// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Backgrounds and masks (phase-2 §3.9.7 "Background", Effects "mask-image"): CSS background
// layers become RmlUi decorators (first layer on top, as in CSS): gradients as RmlUi's native
// gradient decorators with normalized arguments, url() / image-set() images as the compat
// html-layer decorator with all ten arguments (size, position, repeat, origin, clip,
// attachment); background-color stays native. Longhands merge with the rule's shorthand or a
// base rule; gradient text falls back to a solid color.

#include "htmlui/compat/css/props/Rewriters.h"

#include "htmlui/compat/css/BaseRules.h"
#include "htmlui/compat/css/ColorValue.h"
#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/ValueNormalizer.h"

#include <cmath>
#include <functional>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    // where layer diagnostics go and how URLs resolve
    struct LayerScope {
        Session& session;
        uint32_t file = 0;
        SourceRef ref;
        bool mask = false;
        std::function<void(Severity, DiagCode, const std::string&, const std::string&)> report;
    };

    void say(LayerScope& scope, Severity severity, DiagCode code, const std::string& message, const std::string& suggestion = std::string()){
        if (scope.report)
            scope.report(severity, code, message, suggestion);
    }

    // ---- positions

    // one axis of a position: <percentage> + <length> offset (edge offsets: right 10px = 100% - 10px)
    struct Axis {
        double percent = 0;
        std::string length;     // "" or a length ("10px", "-10px")
        std::string raw;        // a length-percentage written as is (var(), em, ...)

        std::string text() const {
            if (!raw.empty())
                return raw;
            if (length.empty())
                return formatNumber(percent) + "%";
            if (percent == 0)
                return length;
            // html-layer reads calc(<percentage> +|- <length>) (HtmlLayerDecorator, 3.11.3)
            return "calc(" + formatNumber(percent) + "% " + (length[0] == '-' ? "- " + length.substr(1) : "+ " + length) + ")";
        }
        // the RmlUi gradient position (no combined offsets)
        std::string gradientText(bool& lost) const {
            if (!raw.empty())
                return raw;
            if (!length.empty() && percent != 0){
                lost = true;
                return formatNumber(percent) + "%";
            }
            if (!length.empty())
                return length;
            return formatNumber(percent) + "%";
        }
    };

    bool negateLength(std::string& length){
        if (length.empty())
            return false;
        if (length[0] == '-')
            length.erase(0, 1);
        else
            length.insert(0, "-");
        return true;
    }

    // CSS <position> (1-4 values) into x and y
    bool parsePosition(Session& session, const std::vector<ComponentValue>& values, Axis& x, Axis& y){
        std::vector<ComponentValue> w = words(values);
        if (w.empty() || w.size() > 4)
            return false;
        auto keyword = [](const ComponentValue& v){ return identOf(v); };
        auto isLP = [&](const ComponentValue& v){ return isLength(session, v, true) || isVar(v); };
        auto setLP = [&](Axis& axis, const ComponentValue& v){
            if (v.isToken(TokenType::Percentage)){
                axis.percent = v.token.number;
            }else if (std::optional<double> px = lengthPx(v)){
                axis.percent = 0;
                axis.length = formatNumber(*px) + "px";
            }else{
                axis.raw = text(v);
            }
        };
        auto edge = [](const std::string& k, bool& horizontal, double& percent) -> bool {
            if (k == "left"){ horizontal = true; percent = 0; return true; }
            if (k == "right"){ horizontal = true; percent = 100; return true; }
            if (k == "top"){ horizontal = false; percent = 0; return true; }
            if (k == "bottom"){ horizontal = false; percent = 100; return true; }
            return false;
        };
        x = Axis();
        y = Axis();
        x.percent = 50;
        y.percent = 50;
        if (w.size() == 1){
            const std::string k = keyword(w[0]);
            bool horizontal = false;
            double percent = 0;
            if (k == "center")
                return true;
            if (edge(k, horizontal, percent)){
                (horizontal ? x : y).percent = percent;
                return true;
            }
            if (isLP(w[0])){
                setLP(x, w[0]);
                return true;
            }
            return false;
        }
        if (w.size() == 2){
            const std::string a = keyword(w[0]), b = keyword(w[1]);
            bool ha = false, hb = false;
            double pa = 50, pb = 50;
            const bool ea = edge(a, ha, pa), eb = edge(b, hb, pb);
            // "top left" / "left top" / "center top" / "50% top" / "left 20%"
            if ((ea || a == "center") && (eb || b == "center")){
                if (ea && eb && ha == hb)
                    return false;
                if ((ea && !ha) || (eb && hb)){
                    // vertical first
                    y.percent = ea ? pa : 50;
                    x.percent = eb ? pb : 50;
                }else{
                    x.percent = ea ? pa : 50;
                    y.percent = eb ? pb : 50;
                }
                return true;
            }
            if ((ea && ha) || a == "center" || isLP(w[0])){
                if (ea)
                    x.percent = pa;
                else if (a == "center")
                    x.percent = 50;
                else
                    setLP(x, w[0]);
                if (eb && !hb)
                    y.percent = pb;
                else if (b == "center")
                    y.percent = 50;
                else if (isLP(w[1]))
                    setLP(y, w[1]);
                else
                    return false;
                return true;
            }
            return false;
        }
        // 3-4 values: edge [offset] edge [offset]
        size_t i = 0;
        bool xSet = false, ySet = false;
        while (i < w.size()){
            const std::string k = keyword(w[i]);
            bool horizontal = false;
            double percent = 50;
            if (k == "center"){
                i++;
                continue;
            }
            if (!edge(k, horizontal, percent))
                return false;
            Axis& axis = horizontal ? x : y;
            if (horizontal ? xSet : ySet)
                return false;
            (horizontal ? xSet : ySet) = true;
            axis = Axis();
            axis.percent = percent;
            i++;
            if (i < w.size() && isLP(w[i])){
                const ComponentValue& offset = w[i];
                if (offset.isToken(TokenType::Percentage)){
                    axis.percent = percent == 100 ? 100 - offset.token.number : offset.token.number;
                }else{
                    std::optional<double> px = lengthPx(offset);
                    std::string length = px ? formatNumber(*px) + "px" : text(offset);
                    if (percent == 100)
                        negateLength(length);
                    axis.length = length;
                }
                i++;
            }
        }
        return true;
    }

    // ---- gradients

    bool isGradientName(const std::string& name){
        return name == "linear-gradient" || name == "radial-gradient" || name == "conic-gradient" || name == "repeating-linear-gradient" ||
               name == "repeating-radial-gradient" || name == "repeating-conic-gradient";
    }

    std::string legacySideToStandard(const std::string& side){
        if (side == "top") return "bottom";
        if (side == "bottom") return "top";
        if (side == "left") return "right";
        if (side == "right") return "left";
        return side;
    }

    // color stops: drop hints, normalize positions; nullopt on a malformed stop
    std::optional<std::string> colorStops(LayerScope& scope, const std::vector<std::vector<ComponentValue>>& groups, size_t first,
                                          bool angular, std::string* middleColor){
        std::vector<std::string> stops;
        std::vector<std::string> colors;
        bool reportedHint = false;
        for (size_t i = first; i < groups.size(); i++){
            std::vector<ComponentValue> w = words(groups[i]);
            if (w.empty())
                return std::nullopt;
            if (w.size() == 1 && (w[0].isToken(TokenType::Percentage) || isLength(scope.session, w[0]) ||
                                  (angular && w[0].isToken(TokenType::Dimension) && unitCategory(w[0].token.unit) == UnitCategory::Angle))){
                if (!reportedHint){
                    reportedHint = true;
                    say(scope, Severity::Info, codes::ApproximatedMinor, "gradient interpolation hints are dropped (the midpoint stays in the middle)");
                }
                continue;
            }
            // color [pos [pos]] (the color may also come after the position)
            std::string color;
            std::vector<std::string> positions;
            for (const ComponentValue& v : w){
                if (color.empty() && (isColor(scope.session, v) || isVar(v))){
                    color = text(v);
                }else if (v.isToken(TokenType::Percentage) || isLength(scope.session, v) ||
                          (angular && v.isToken(TokenType::Dimension) && unitCategory(v.token.unit) == UnitCategory::Angle)){
                    positions.push_back(v.isToken(TokenType::Number) ? "0%" : text(v));
                }else{
                    return std::nullopt;
                }
            }
            if (color.empty() || positions.size() > 2)
                return std::nullopt;
            colors.push_back(color);
            std::string stop = color;
            for (const std::string& p : positions)
                stop += " " + p;
            stops.push_back(stop);
        }
        if (stops.size() < 2 && !(stops.size() == 1))
            return std::nullopt;
        if (middleColor && !colors.empty())
            *middleColor = colors[colors.size() / 2];
        return join(stops, ", ");
    }

    // a gradient function as RmlUi's decorator; nullopt when it cannot be written
    std::optional<std::string> gradient(LayerScope& scope, const ComponentValue& function, std::string* middleColor){
        std::string name = lowerAscii(function.name());
        bool legacy = false;
        for (const char* prefix : {"-webkit-", "-moz-", "-o-", "-ms-"}){
            const std::string p(prefix);
            if (name.compare(0, p.size(), p) == 0){
                name = name.substr(p.size());
                legacy = true;
            }
        }
        if (!isGradientName(name))
            return std::nullopt;
        if (legacy)
            say(scope, Severity::Info, codes::VendorPrefix, "'" + lowerAscii(function.name()) + "()' is read as '" + name + "()'");
        std::vector<std::vector<ComponentValue>> groups = commaGroups(function.children);
        if (groups.empty())
            return std::nullopt;
        const bool linear = name.find("linear") != std::string::npos;
        const bool radial = name.find("radial") != std::string::npos;
        const bool conic = name.find("conic") != std::string::npos;

        // a color-interpolation method ("in oklch") anywhere in the first group is dropped
        std::vector<ComponentValue> head = words(groups[0]);
        bool interpolation = false;
        for (size_t i = 0; i + 1 < head.size(); i++){
            if (isIdent(head[i], "in")){
                interpolation = true;
                say(scope, Severity::Info, codes::ApproximatedMinor, "gradient color interpolation '" + text(head[i + 1]) +
                    "' is dropped (sRGB interpolation)");
                size_t end = i + 2;
                if (end + 1 < head.size() && isIdent(head[end + 1], "hue"))
                    end += 2;
                head.erase(head.begin() + (std::ptrdiff_t)i, head.begin() + (std::ptrdiff_t)std::min(end, head.size()));
                break;
            }
        }
        bool headIsStop = false;
        if (!head.empty() && (isColor(scope.session, head[0]) || isVar(head[0])))
            headIsStop = true;
        if (head.empty() && groups[0].size() > 0)
            headIsStop = false;
        std::string prefix;
        bool lostOffset = false;
        if (!headIsStop && !head.empty()){
            if (linear){
                if (head.size() == 1 && head[0].isToken(TokenType::Dimension) && unitCategory(head[0].token.unit) == UnitCategory::Angle){
                    double angle = head[0].token.number;
                    if (legacy)
                        angle = std::fmod(90.0 - angle + 360.0 * 4, 360.0);
                    prefix = formatNumber(angle) + "deg";
                }else if (head.size() == 1 && head[0].isToken(TokenType::Number) && head[0].token.number == 0){
                    prefix = legacy ? "90deg" : "0deg";
                }else if (isIdent(head[0], "to") && !legacy){
                    std::vector<std::string> sides;
                    for (size_t i = 1; i < head.size(); i++)
                        sides.push_back(identOf(head[i]));
                    prefix = "to " + join(sides);
                }else if (legacy){
                    std::vector<std::string> sides;
                    for (const ComponentValue& v : head){
                        const std::string k = identOf(v);
                        if (k == "center")
                            continue;
                        if (k != "top" && k != "bottom" && k != "left" && k != "right")
                            return std::nullopt;
                        sides.push_back(legacySideToStandard(k));
                    }
                    if (!sides.empty())
                        prefix = "to " + join(sides);
                }else{
                    return std::nullopt;
                }
            }else if (radial){
                // [shape] [size] [at position] in RmlUi's order; legacy: <position>, <shape> <size>
                std::string shape, sizeX, sizeY;
                std::vector<ComponentValue> position;
                std::vector<ComponentValue> shapeWords = head;
                if (legacy && groups.size() > 2){
                    // -webkit-radial-gradient(center, circle cover, ...)
                    std::vector<ComponentValue> second = words(groups[1]);
                    bool secondIsShape = !second.empty() && !isColor(scope.session, second[0]) && !isVar(second[0]);
                    position = head;
                    shapeWords = secondIsShape ? second : std::vector<ComponentValue>();
                    if (secondIsShape)
                        groups.erase(groups.begin() + 1);
                }
                bool atWord = false;
                for (size_t i = 0; i < shapeWords.size(); i++){
                    const ComponentValue& v = shapeWords[i];
                    const std::string k = identOf(v);
                    if (atWord){
                        position.push_back(v);
                        continue;
                    }
                    if (k == "at"){
                        atWord = true;
                        continue;
                    }
                    if (k == "circle" || k == "ellipse")
                        shape = k;
                    else if (k == "closest-side" || k == "farthest-side" || k == "closest-corner" || k == "farthest-corner")
                        sizeX = k;
                    else if (k == "cover")
                        sizeX = "farthest-corner";
                    else if (k == "contain")
                        sizeX = "closest-side";
                    else if (isLength(scope.session, v, true)){
                        (sizeX.empty() ? sizeX : sizeY) = v.isToken(TokenType::Number) ? "0px" : text(v);
                    }else
                        return std::nullopt;
                }
                std::vector<std::string> parts;
                if (!shape.empty())
                    parts.push_back(shape);
                if (!sizeX.empty())
                    parts.push_back(sizeX);
                if (!sizeY.empty())
                    parts.push_back(sizeY);
                if (!position.empty()){
                    Axis x, y;
                    if (!parsePosition(scope.session, position, x, y))
                        return std::nullopt;
                    parts.push_back("at " + x.gradientText(lostOffset) + " " + y.gradientText(lostOffset));
                }
                prefix = join(parts);
            }else if (conic){
                std::vector<std::string> parts;
                std::vector<ComponentValue> position;
                bool at = false;
                for (size_t i = 0; i < head.size(); i++){
                    const ComponentValue& v = head[i];
                    if (at){
                        position.push_back(v);
                        continue;
                    }
                    if (isIdent(v, "from") && i + 1 < head.size()){
                        const ComponentValue& angle = head[i + 1];
                        if (angle.isToken(TokenType::Number) && angle.token.number == 0)
                            parts.push_back("from 0deg");
                        else if (angle.isToken(TokenType::Dimension) && unitCategory(angle.token.unit) == UnitCategory::Angle)
                            parts.push_back("from " + text(angle));
                        else
                            return std::nullopt;
                        i++;
                        continue;
                    }
                    if (isIdent(v, "at")){
                        at = true;
                        continue;
                    }
                    return std::nullopt;
                }
                if (!position.empty()){
                    Axis x, y;
                    if (!parsePosition(scope.session, position, x, y))
                        return std::nullopt;
                    parts.push_back("at " + x.gradientText(lostOffset) + " " + y.gradientText(lostOffset));
                }
                prefix = join(parts);
            }
        }
        if (lostOffset)
            say(scope, Severity::Info, codes::ApproximatedMinor, "a gradient position offset from an edge is dropped (the edge is used)");
        std::optional<std::string> stops = colorStops(scope, groups, headIsStop || (head.empty() && !interpolation) ? 0 : 1, conic, middleColor);
        if (!stops)
            return std::nullopt;
        return name + "(" + (prefix.empty() ? std::string() : prefix + ", ") + *stops + ")";
    }

    // ---- layers

    struct Layer {
        enum class Kind { None, Gradient, Image, Var } kind = Kind::None;
        std::string image;            // the gradient decorator, the image path, or the var() reference
        std::string middleColor;      // a gradient's middle stop (gradient text)
        Axis x, y;
        bool positionSet = false;
        std::string sizeX = "auto", sizeY = "auto";
        bool sizeSet = false;
        std::string repeatX = "repeat", repeatY = "repeat";
        std::string attachment = "scroll";
        // CSS's default clip is the border box, painted beneath the border; RmlUi draws
        // decorators above the border, so the layer would hide it: by default the layer fills
        // the padding box (what an opaque border leaves visible). An explicit
        // background-clip: border-box is kept.
        std::string origin = "padding-box", clip = "padding-box";
        bool clipText = false;
        SourceRef ref;
    };

    struct Layers {
        std::vector<Layer> layers;
        std::string color;            // background-color of the shorthand ("" = transparent)
        bool colorSet = false;
    };

    // an image-set() candidate for the dp ratio
    std::optional<ComponentValue> imageSetCandidate(Session& session, const ComponentValue& function){
        struct Candidate { ComponentValue image; double resolution; };
        std::vector<Candidate> candidates;
        for (const std::vector<ComponentValue>& group : commaGroups(function.children)){
            std::vector<ComponentValue> w = words(group);
            if (w.empty())
                continue;
            Candidate c{w[0], 1.0};
            for (size_t i = 1; i < w.size(); i++){
                if (w[i].isToken(TokenType::Dimension) && unitCategory(w[i].token.unit) == UnitCategory::Resolution)
                    c.resolution = w[i].token.number * canonicalFactor(w[i].token.unit).value_or(1.0);
                else if (w[i].isFunction("type")){
                    const std::string type = lowerAscii(text(w[i].children));
                    if (type.find("webp") != std::string::npos || type.find("avif") != std::string::npos)
                        c.resolution = -1;   // a format the engine does not decode
                }
            }
            if (c.resolution > 0)
                candidates.push_back(c);
        }
        if (candidates.empty())
            return std::nullopt;
        const double ratio = session.options.dpRatio;
        const Candidate* best = nullptr;
        for (const Candidate& c : candidates){
            if (c.resolution >= ratio && (!best || c.resolution < best->resolution || best->resolution < ratio))
                best = &c;
        }
        if (!best){
            for (const Candidate& c : candidates){
                if (!best || c.resolution > best->resolution)
                    best = &c;
            }
        }
        return best->image;
    }

    // the image of a layer: true when the component is an image (set into layer)
    bool layerImage(LayerScope& scope, const ComponentValue& v, Layer& layer, bool& dropped){
        dropped = false;
        if (isIdent(v, "none")){
            layer.kind = Layer::Kind::None;
            return true;
        }
        if (v.isToken(TokenType::Url) || v.isFunction("url") || v.isFunction("src")){
            std::optional<std::string> url = urlOf(v);
            const SourceRef ref = v.length > 0 ? SourceRef{scope.file, v.offset, v.length} : scope.ref;
            const std::string path = url ? resolveUrlIn(scope.session, *url, scope.file, ref, "image") : std::string();
            if (path.empty()){
                dropped = true;
                return true;
            }
            layer.kind = Layer::Kind::Image;
            layer.image = path;
            return true;
        }
        if (v.kind != ComponentValue::Kind::Function)
            return false;
        const std::string name = lowerAscii(v.name());
        if (name == "image-set" || name == "-webkit-image-set"){
            std::optional<ComponentValue> candidate = imageSetCandidate(scope.session, v);
            if (!candidate){
                say(scope, Severity::Warning, codes::UnsupportedFunction, "image-set() has no candidate the engine can draw; layer dropped",
                    "list a PNG or JPEG candidate");
                dropped = true;
                return true;
            }
            return layerImage(scope, *candidate, layer, dropped);
        }
        if (name == "var"){
            // a static variable is read as what it holds; a dynamic one is written as is
            std::vector<ComponentValue> replacement;
            if (scope.session.staticVar(v, replacement)){
                std::vector<ComponentValue> w = words(replacement);
                if (w.size() == 1 && layerImage(scope, w[0], layer, dropped))
                    return true;
                // a variable holding a color is the final layer's color, not its image
                // (background: var(--bg) url(tile.png) repeat)
                if (w.size() == 1 && isColor(scope.session, w[0]))
                    return false;
            }
            layer.kind = Layer::Kind::Var;
            layer.image = text(v);
            return true;
        }
        if (name == "-webkit-gradient"){
            say(scope, Severity::Warning, codes::UnsupportedFunction, "the 2008 -webkit-gradient() syntax is not supported; layer dropped",
                "use linear-gradient() or radial-gradient()");
            dropped = true;
            return true;
        }
        if (name == "element" || name == "cross-fade" || name == "-webkit-cross-fade" || name == "paint" || name == "image"){
            say(scope, Severity::Warning, codes::UnsupportedFunction, "'" + name + "()' is not supported; layer dropped", "use url() or a gradient");
            dropped = true;
            return true;
        }
        std::string plain = name;
        for (const char* prefix : {"-webkit-", "-moz-", "-o-", "-ms-"}){
            const std::string p(prefix);
            if (plain.compare(0, p.size(), p) == 0)
                plain = plain.substr(p.size());
        }
        if (isGradientName(plain)){
            std::optional<std::string> g = gradient(scope, v, &layer.middleColor);
            if (!g){
                say(scope, Severity::Warning, codes::UnsupportedValue, "'" + text(v) + "' cannot be written as a gradient decorator; layer dropped");
                dropped = true;
                return true;
            }
            layer.kind = Layer::Kind::Gradient;
            layer.image = *g;
            return true;
        }
        return false;
    }

    bool isRepeat(const std::string& k){
        return k == "repeat" || k == "no-repeat" || k == "space" || k == "round" || k == "repeat-x" || k == "repeat-y";
    }
    bool isBox(const std::string& k){
        return k == "border-box" || k == "padding-box" || k == "content-box" || k == "text";
    }

    void setRepeat(Layer& layer, const std::vector<std::string>& values){
        if (values.size() == 1){
            if (values[0] == "repeat-x"){
                layer.repeatX = "repeat";
                layer.repeatY = "no-repeat";
            }else if (values[0] == "repeat-y"){
                layer.repeatX = "no-repeat";
                layer.repeatY = "repeat";
            }else{
                layer.repeatX = layer.repeatY = values[0];
            }
        }else if (values.size() == 2){
            layer.repeatX = values[0];
            layer.repeatY = values[1];
        }
    }

    bool setSize(Session& session, Layer& layer, const std::vector<ComponentValue>& w){
        if (w.size() == 1 && (isIdent(w[0], "cover") || isIdent(w[0], "contain"))){
            layer.sizeX = layer.sizeY = identOf(w[0]);
            layer.sizeSet = true;
            return true;
        }
        if (w.empty() || w.size() > 2)
            return false;
        std::string values[2] = {"auto", "auto"};
        for (size_t i = 0; i < w.size(); i++){
            if (isIdent(w[i], "auto"))
                values[i] = "auto";
            else if (isLength(session, w[i], true) || isVar(w[i]))
                values[i] = w[i].isToken(TokenType::Number) ? "0px" : text(w[i]);
            else
                return false;
        }
        layer.sizeX = values[0];
        layer.sizeY = values[1];
        layer.sizeSet = !(values[0] == "auto" && values[1] == "auto");
        return true;
    }

    // one layer of the background / mask shorthand
    bool parseLayer(LayerScope& scope, const std::vector<ComponentValue>& group, bool last, Layer& layer, Layers& out, bool& dropped){
        dropped = false;
        std::vector<ComponentValue> w = words(group);
        std::vector<ComponentValue> position;
        std::vector<std::string> repeat;
        std::vector<std::string> boxes;
        bool imageSet = false;
        for (size_t i = 0; i < w.size(); i++){
            const ComponentValue& v = w[i];
            const std::string k = identOf(v);
            if (v.isToken(TokenType::Delim) && v.token.isDelim('/')){
                // position / size
                std::vector<ComponentValue> size;
                size_t j = i + 1;
                while (j < w.size() && (isLength(scope.session, w[j], true) || isIdent(w[j], "auto") || isIdent(w[j], "cover") ||
                                        isIdent(w[j], "contain")) && size.size() < 2){
                    size.push_back(w[j]);
                    j++;
                }
                if (position.empty() || !setSize(scope.session, layer, size))
                    return false;
                i = j - 1;
                continue;
            }
            if (!imageSet){
                bool imageDropped = false;
                if (layerImage(scope, v, layer, imageDropped)){
                    imageSet = true;
                    if (imageDropped)
                        dropped = true;
                    continue;
                }
            }
            if (isRepeat(k)){
                repeat.push_back(k);
                continue;
            }
            if (k == "scroll" || k == "fixed" || k == "local"){
                if (k == "local")
                    say(scope, Severity::Info, codes::ApproximatedMinor, "'background-attachment: local' is drawn as scroll");
                layer.attachment = k == "local" ? "scroll" : k;
                continue;
            }
            if (isBox(k)){
                boxes.push_back(k);
                continue;
            }
            if (k == "left" || k == "right" || k == "top" || k == "bottom" || k == "center" ||
                isLength(scope.session, v, true) || (isVar(v) && imageSet)){
                position.push_back(v);
                continue;
            }
            if (last && (isColor(scope.session, v) || isVar(v)) && !out.colorSet){
                out.color = text(v);
                out.colorSet = true;
                continue;
            }
            return false;
        }
        if (!position.empty()){
            if (!parsePosition(scope.session, position, layer.x, layer.y))
                return false;
            layer.positionSet = true;
        }
        if (!repeat.empty())
            setRepeat(layer, repeat);
        if (boxes.size() >= 1){
            if (boxes[0] == "text")
                layer.clipText = true;
            else
                layer.origin = boxes[0];
            if (boxes.size() >= 2){
                if (boxes[1] == "text")
                    layer.clipText = true;
                else
                    layer.clip = boxes[1];
            }else if (boxes[0] != "text"){
                layer.clip = boxes[0];
            }
        }
        return true;
    }

    // the decorator text of the layers ("" when none)
    std::string decoratorText(LayerScope& scope, const std::vector<Layer>& layers){
        std::vector<std::string> out;
        bool reportedPattern = false;
        for (const Layer& layer : layers){
            switch (layer.kind){
                case Layer::Kind::None:
                    break;
                case Layer::Kind::Var:
                    out.push_back(layer.image);
                    break;
                case Layer::Kind::Gradient: {
                    const bool fullBox = !layer.sizeSet && (!layer.positionSet || (layer.x.text() == "0%" && layer.y.text() == "0%"));
                    if (!fullBox && !reportedPattern){
                        reportedPattern = true;
                        say(scope, Severity::Warning, codes::ApproximatedVisible,
                            "a gradient layer with a background-size or background-position is drawn once over the whole box",
                            "draw the pattern with an image");
                    }
                    out.push_back(layer.image + " " + layer.clip);
                    break;
                }
                case Layer::Kind::Image:
                    out.push_back("html-layer(" + serializeString(layer.image) + " " + layer.sizeX + " " + layer.sizeY + " " + layer.x.text() + " " +
                                  layer.y.text() + " " + layer.repeatX + " " + layer.repeatY + " " + layer.origin + " " + layer.clip + " " +
                                  layer.attachment + ")");
                    break;
            }
        }
        return join(out, ", ");
    }

    // ---- longhands: lists applied to the image layers (CSS repetition)

    bool isBackgroundFamily(const std::string& name, bool mask){
        if (mask){
            if (name == "clip-path")
                return true;
            for (const char* prefix : {"mask", "-webkit-mask"}){
                const std::string p(prefix);
                if (name == p || name.compare(0, p.size() + 1, p + "-") == 0)
                    return true;
            }
            return false;
        }
        return name == "background" || name == "background-image" || name == "background-position" || name == "background-position-x" ||
               name == "background-position-y" || name == "background-size" || name == "background-repeat" || name == "background-origin" ||
               name == "background-clip" || name == "background-attachment" || name == "-webkit-background-clip" ||
               name == "-webkit-background-size" || name == "-webkit-background-origin";
    }

    std::string plainName(const std::string& name){
        for (const char* prefix : {"-webkit-", "-moz-"}){
            const std::string p(prefix);
            if (name.compare(0, p.size(), p) == 0)
                return name.substr(p.size());
        }
        return name;
    }

    // applies one declaration of the family to the layers; false when the value cannot be read
    bool applyDeclaration(LayerScope& scope, const std::string& rawName, const std::vector<ComponentValue>& value, Layers& state){
        std::string name = plainName(rawName);
        if (scope.mask && name.compare(0, 5, "mask-") == 0)
            name = "background-" + name.substr(5);
        else if (scope.mask && name == "mask")
            name = "background";
        std::vector<std::vector<ComponentValue>> groups = commaGroups(value);
        if (name == "background"){
            Layers fresh;
            for (size_t i = 0; i < groups.size(); i++){
                Layer layer;
                bool dropped = false;
                if (!parseLayer(scope, groups[i], i + 1 == groups.size(), layer, fresh, dropped)){
                    if (scope.mask){
                        // mask-mode, mask-composite parts of the shorthand
                        say(scope, Severity::Info, codes::UnsupportedValue, "mask shorthand parts beyond image, position, size, repeat, origin and clip are dropped");
                    }
                    return false;
                }
                if (!dropped)
                    fresh.layers.push_back(layer);
            }
            state = fresh;
            if (!state.colorSet){
                state.colorSet = true;
                state.color.clear();
            }
            return true;
        }
        if (name == "background-image"){
            std::vector<Layer> previous = state.layers;
            state.layers.clear();
            for (size_t i = 0; i < groups.size(); i++){
                Layer layer = i < previous.size() ? previous[i] : (previous.empty() ? Layer() : previous[i % previous.size()]);
                std::vector<ComponentValue> w = words(groups[i]);
                bool dropped = false;
                if (w.size() != 1 || !layerImage(scope, w[0], layer, dropped))
                    return false;
                if (!dropped)
                    state.layers.push_back(layer);
            }
            return true;
        }
        // the per-layer longhands repeat over the layers
        if (state.layers.empty())
            state.layers.push_back(Layer());
        for (size_t i = 0; i < state.layers.size(); i++){
            const std::vector<ComponentValue> w = words(groups[i % groups.size()]);
            Layer& layer = state.layers[i];
            if (name == "background-position"){
                if (!parsePosition(scope.session, w, layer.x, layer.y))
                    return false;
                layer.positionSet = true;
            }else if (name == "background-position-x" || name == "background-position-y"){
                // one axis: read as a position with the other axis centered
                const bool horizontal = name == "background-position-x";
                std::vector<ComponentValue> position;
                if (!horizontal){
                    position.push_back(ValueNormalizer::ident("center"));
                    position.push_back(ValueNormalizer::whitespace());
                }
                for (const ComponentValue& v : w){
                    position.push_back(v);
                    position.push_back(ValueNormalizer::whitespace());
                }
                if (horizontal)
                    position.push_back(ValueNormalizer::ident("center"));
                Axis x, y;
                if (w.empty() || !parsePosition(scope.session, position, x, y))
                    return false;
                (horizontal ? layer.x : layer.y) = horizontal ? x : y;
                layer.positionSet = true;
            }else if (name == "background-size"){
                if (!setSize(scope.session, layer, w))
                    return false;
            }else if (name == "background-repeat"){
                std::vector<std::string> repeat;
                for (const ComponentValue& v : w){
                    if (!isRepeat(identOf(v)))
                        return false;
                    repeat.push_back(identOf(v));
                }
                setRepeat(layer, repeat);
            }else if (name == "background-origin"){
                if (w.size() != 1 || !isBox(identOf(w[0])) || identOf(w[0]) == "text")
                    return false;
                layer.origin = identOf(w[0]);
            }else if (name == "background-clip"){
                if (w.size() != 1 || !isBox(identOf(w[0])))
                    return false;
                if (identOf(w[0]) == "text")
                    layer.clipText = true;
                else
                    layer.clip = identOf(w[0]);
            }else if (name == "background-attachment"){
                const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
                if (k != "scroll" && k != "fixed" && k != "local")
                    return false;
                layer.attachment = k == "local" ? "scroll" : k;
            }else{
                return false;
            }
        }
        return true;
    }

    bool hasImageDeclaration(const FlatRule& rule, bool mask){
        for (const Declaration& d : rule.declarations){
            const std::string n = plainName(d.name);
            if (!mask && (n == "background" || n == "background-image"))
                return true;
            if (mask && (n == "mask" || n == "mask-image"))
                return true;
        }
        return false;
    }

    // the normalized value of a declaration of another rule (base rules)
    std::vector<ComponentValue> normalized(Session& session, const Declaration& d){
        NormalizeContext context;
        context.property = d.name;
        context.options = session.options;
        context.resolveVar = [&session](const ComponentValue& var, std::vector<ComponentValue>& out){ return session.staticVar(var, out); };
        NormalizeResult result = ValueNormalizer::normalize(d.value, context);
        return result.ok ? trimWhitespace(result.value) : std::vector<ComponentValue>();
    }

    // ---- gradient text

    bool transparentFill(Session& session, const FlatRule& rule){
        for (const char* property : {"-webkit-text-fill-color", "color"}){
            std::optional<std::vector<ComponentValue>> value = ruleValue(session, rule, property);
            if (value && value->size() == 1 && (*value)[0].isToken(TokenType::Hash)){
                const std::string& hex = (*value)[0].token.value;
                if (hex.size() == 8 && hex.compare(6, 2, "00") == 0)
                    return true;
            }
        }
        return false;
    }

    LayerScope scopeOf(DeclContext& context, bool mask){
        LayerScope scope{context.session, context.declaration.ref.file, context.declaration.ref, mask, {}};
        scope.report = [&context](Severity severity, DiagCode code, const std::string& message, const std::string& suggestion){
            context.reportAs(severity, code, message, suggestion);
        };
        return scope;
    }

    // the family of the rule, merged at its last declaration (and with the base rule)
    void rewriteLayers(DeclContext& context, bool mask){
        LayerScope scope = scopeOf(context, mask);
        Layers state;
        if (context.rule && !context.keyframe){
            const Declaration* last = nullptr;
            for (const Declaration& d : context.rule->declarations){
                if (isBackgroundFamily(d.name, mask) && reachesRewrite(context.session, d, context.rule->declarations))
                    last = &d;
            }
            if (last && last->ref != context.declaration.ref)
                return;
            if (!hasImageDeclaration(*context.rule, mask)){
                const bool onlyClipPath = [&](){
                    for (const Declaration& d : context.rule->declarations){
                        if (isBackgroundFamily(d.name, mask) && d.name != "clip-path")
                            return false;
                    }
                    return true;
                }();
                if (!onlyClipPath){
                    const FlatRule* base = BaseRules::find(context.session, *context.rule, [mask](const FlatRule& candidate){
                        return hasImageDeclaration(candidate, mask);
                    });
                    if (base){
                        LayerScope quiet = scope;
                        quiet.report = nullptr;
                        quiet.file = base->ref.file;
                        for (const Declaration& d : base->declarations){
                            if (isBackgroundFamily(d.name, mask) && d.name != "clip-path" && reachesRewrite(context.session, d, base->declarations))
                                applyDeclaration(quiet, d.name, normalized(context.session, d), state);
                        }
                        context.reportAs(Severity::Info, codes::MergedFromBaseRule, std::string(mask ? "mask" : "background") +
                                         " longhands are merged with the layers of the base rule at " + BaseRules::location(context.session, *base));
                    }else{
                        context.reportAs(Severity::Warning, codes::LonghandWithoutBase, "'" + context.name + "' without " +
                                         (mask ? "a mask image" : "a background image") + " in the rule or a base rule is dropped",
                                         std::string("set ") + (mask ? "mask-image" : "background-image") + " in the same rule");
                        context.dropped = true;
                        return;
                    }
                }
            }
            bool applied = false, attempted = false;
            for (const Declaration& d : context.rule->declarations){
                if (!isBackgroundFamily(d.name, mask) || d.name == "clip-path" || !reachesRewrite(context.session, d, context.rule->declarations))
                    continue;
                std::vector<ComponentValue> value = d.ref == context.declaration.ref ? context.value : normalized(context.session, d);
                if (value.size() == 1 && isVar(value[0])){
                    applied = true;
                    continue;   // a whole-value var() is handled below for its own declaration
                }
                attempted = true;
                if (applyDeclaration(scope, d.name, value, state))
                    applied = true;
                else
                    context.reportAt(d.ref, Severity::Warning, codes::UnsupportedValue, "'" + d.name + ": " + text(value) + "' is not supported; dropped",
                                     "check the background syntax");
            }
            if (attempted && !applied && !(mask && context.name == "clip-path")){
                context.dropped = true;
                return;
            }
        }else{
            if (!(context.value.size() == 1 && isVar(context.value[0])) && !applyDeclaration(scope, context.name, context.value, state)){
                context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.sourceText() + "' is not supported; declaration dropped",
                                 "check the background syntax");
                context.dropped = true;
                return;
            }
        }

        // a whole-value var() (the last declaration of the family)
        if (context.value.size() == 1 && isVar(context.value[0]) && (plainName(context.name) == "background" || plainName(context.name) == "background-image" ||
                                                                      plainName(context.name) == "mask" || plainName(context.name) == "mask-image")){
            std::optional<TypedVar> typed = wholeVar(context, mask ? "mask" : "decorator");
            if (!typed){
                context.emit(mask ? "mask-image" : "decorator", context.valueText());
                return;
            }
            if (typed->kind == "color"){
                context.emit("background-color", typed->reference);
                companions::emitBackgroundLayers(context, std::string());
            }else{
                if (!mask && plainName(context.name) == "background")
                    context.emit("background-color", "transparent");
                if (mask)
                    context.emit("mask-image", typed->reference);
                else
                    companions::emitBackgroundLayers(context, typed->reference);
            }
            return;
        }

        // gradient text: background-clip: text with a transparent text fill
        bool clipText = false;
        std::string middle;
        for (const Layer& layer : state.layers){
            if (layer.clipText){
                clipText = true;
                if (middle.empty())
                    middle = layer.kind == Layer::Kind::Gradient ? layer.middleColor : "#ffffffff";
            }
        }
        if (clipText && !mask){
            const FlatRule* base = context.rule ? BaseRules::find(context.session, *context.rule, [&](const FlatRule& candidate){
                return transparentFill(context.session, candidate);
            }) : nullptr;
            if (context.rule && (transparentFill(context.session, *context.rule) || base)){
                if (middle.empty())
                    middle = "#ffffffff";
                context.reportAs(Severity::Warning, codes::ApproximatedVisible, "gradient text is drawn in a solid color (" + middle + ")",
                                 "use a solid text color");
                if (state.colorSet)
                    context.emit("background-color", state.color.empty() ? "transparent" : state.color);
                companions::emitBackgroundLayers(context, std::string());
                context.emit("color", middle);
                return;
            }
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'background-clip: text' without a transparent text color is dropped");
            for (Layer& layer : state.layers)
                layer.clipText = false;
        }

        const std::string decorators = decoratorText(scope, state.layers);
        if (mask){
            // clip-path shapes join the mask list
            std::string shapes;
            if (context.rule){
                for (const Declaration& d : context.rule->declarations){
                    if (d.name != "clip-path")
                        continue;
                    std::optional<std::string> shape = clipPathShape(context, d);
                    if (shape)
                        shapes = *shape;
                }
            }
            std::string all = decorators;
            if (!shapes.empty())
                all = all.empty() ? shapes : all + ", " + shapes;
            context.emit("mask-image", all.empty() ? "none" : all);
            return;
        }
        if (state.colorSet)
            context.emit("background-color", state.color.empty() ? "transparent" : state.color);
        companions::emitBackgroundLayers(context, decorators);
    }

    // ---- var() families

    std::string classifyLayers(Session& session, const std::vector<ComponentValue>& value, bool mask){
        std::vector<ComponentValue> w = words(value);
        if (w.size() == 1 && isColor(session, w[0]))
            return mask ? std::string() : "color";
        LayerScope scope{session, 0, SourceRef(), mask, {}};
        Layers state;
        if (!applyDeclaration(scope, "background", value, state))
            return std::string();
        if (state.colorSet && !state.color.empty())
            return std::string();   // layers and a color: two RCSS properties
        return "layers";
    }

    std::optional<std::string> transformLayers(Session& session, const std::vector<ComponentValue>& value, const std::string& kind,
                                               SourceRef ref, bool mask){
        if (kind == "color")
            return text(value);
        LayerScope scope{session, ref.file, ref, mask, {}};
        scope.report = [&session, ref](Severity severity, DiagCode code, const std::string& message, const std::string& suggestion){
            session.sink->reportAs(severity, code, ref, message, suggestion);
        };
        Layers state;
        if (!applyDeclaration(scope, "background", value, state))
            return std::nullopt;
        const std::string decorators = decoratorText(scope, state.layers);
        return decorators.empty() ? std::string(companions::kNoLayer) : decorators;
    }

    std::string classifyDecorator(Session& session, const std::vector<ComponentValue>& value){ return classifyLayers(session, value, false); }
    std::string classifyMask(Session& session, const std::vector<ComponentValue>& value){ return classifyLayers(session, value, true); }
    std::optional<std::string> transformDecorator(Session& session, const std::vector<ComponentValue>& value, const std::string& kind, SourceRef ref){
        return transformLayers(session, value, kind, ref, false);
    }
    std::optional<std::string> transformMask(Session& session, const std::vector<ComponentValue>& value, const std::string& kind, SourceRef ref){
        return transformLayers(session, value, kind, ref, true);
    }

}

const VarFamily jasperengine::htmlui::compat::kDecoratorFamily = {"decorator", classifyDecorator, transformDecorator};
const VarFamily jasperengine::htmlui::compat::kMaskFamily = {"mask", classifyMask, transformMask};

void jasperengine::htmlui::compat::rewriteBackground(DeclContext& context){
    rewriteLayers(context, false);
}

void jasperengine::htmlui::compat::rewriteBackgroundLonghand(DeclContext& context){
    rewriteLayers(context, false);
}

void jasperengine::htmlui::compat::rewriteMaskImage(DeclContext& context){
    rewriteLayers(context, true);
}

void jasperengine::htmlui::compat::rewriteClipPath(DeclContext& context){
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && isIdent(w[0], "none")){
        if (!context.rule)
            context.emit("mask-image", "none");
        else
            rewriteLayers(context, true);
        return;
    }
    if (!context.rule){
        std::optional<std::string> shape = clipPathShape(context, context.declaration);
        if (shape)
            context.emit("mask-image", *shape);
        return;
    }
    rewriteLayers(context, true);
}
