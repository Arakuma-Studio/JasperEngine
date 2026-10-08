// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// §3.7.3 steps 6 and 8: every element through the element table (Appendix D), the image
// choices (srcset, picture), SVG images, web-component Lottie players, placeholders for what
// the engine cannot draw; inline SVG passes through (foreignObject dropped).

#include "htmlui/compat/html/HtmlTransform.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/UrlResolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <limits>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::html;

namespace {

    // ---- media conditions of sizes="" and <source media> (static, at load) ----

    // set when an evaluation read the viewport (the compile then depends on it, §3.12.2)
    thread_local bool viewportUsed = false;

    double lengthToPx(const std::string& value, const CompatOptions& options, bool& ok){
        ok = false;
        char* end = nullptr;
        const double number = std::strtod(value.c_str(), &end);
        if (end == value.c_str())
            return 0.0;
        const std::string unit = toLower(trim(std::string(end)));
        ok = true;
        if (unit.empty() || unit == "px")
            return number;
        if (unit == "em" || unit == "rem")
            return number * 16.0;
        if (unit == "vw" || unit == "vh" || unit == "vmin" || unit == "vmax")
            viewportUsed = true;
        if (unit == "vw")
            return number * options.viewport.x / 100.0;
        if (unit == "vh")
            return number * options.viewport.y / 100.0;
        if (unit == "vmin")
            return number * std::min(options.viewport.x, options.viewport.y) / 100.0;
        if (unit == "vmax")
            return number * std::max(options.viewport.x, options.viewport.y) / 100.0;
        if (unit == "in")
            return number * 96.0;
        if (unit == "cm")
            return number * 96.0 / 2.54;
        if (unit == "mm")
            return number * 96.0 / 25.4;
        if (unit == "pt")
            return number * 96.0 / 72.0;
        ok = false;
        return 0.0;
    }

    bool evaluateFeature(std::string feature, const CompatOptions& options){
        feature = trim(feature);
        // range syntax: (width >= 600px), (600px <= width)
        static const char* const operators[] = {">=", "<=", ">", "<", "="};
        for (const char* op : operators){
            const size_t at = feature.find(op);
            if (at == std::string::npos)
                continue;
            std::string left = trim(feature.substr(0, at));
            std::string right = trim(feature.substr(at + std::string(op).size()));
            std::string name = left;
            std::string value = right;
            std::string effective = op;
            if (left != "width" && left != "height"){
                // value op name: mirror the operator
                name = right;
                value = left;
                if (effective == ">=") effective = "<=";
                else if (effective == "<=") effective = ">=";
                else if (effective == ">") effective = "<";
                else if (effective == "<") effective = ">";
            }
            if (name != "width" && name != "height")
                return false;
            viewportUsed = true;
            bool ok = false;
            const double px = lengthToPx(value, options, ok);
            if (!ok)
                return false;
            const double size = (name == "width") ? options.viewport.x : options.viewport.y;
            if (effective == ">=") return size >= px;
            if (effective == "<=") return size <= px;
            if (effective == ">") return size > px;
            if (effective == "<") return size < px;
            return size == px;
        }
        const size_t colon = feature.find(':');
        if (colon == std::string::npos)
            return false;   // boolean features: none is statically true here
        const std::string name = toLower(trim(feature.substr(0, colon)));
        const std::string value = toLower(trim(feature.substr(colon + 1)));
        bool ok = false;
        if (name == "min-width" || name == "max-width" || name == "min-height" || name == "max-height"){
            viewportUsed = true;
            const double px = lengthToPx(value, options, ok);
            if (!ok)
                return false;
            const double size = (name.find("width") != std::string::npos) ? options.viewport.x : options.viewport.y;
            return (name[1] == 'i') ? size >= px : size <= px;
        }
        if (name == "orientation")
            viewportUsed = true;
        if (name == "orientation")
            return value == (options.viewport.x >= options.viewport.y ? "landscape" : "portrait");
        if (name == "prefers-color-scheme")
            return value == options.colorScheme;
        if (name == "prefers-reduced-motion")
            return value == (options.reducedMotion ? "reduce" : "no-preference");
        if (name == "min-resolution" || name == "max-resolution" || name == "resolution"){
            double dppx = std::strtod(value.c_str(), nullptr);
            if (value.find("dpi") != std::string::npos)
                dppx /= 96.0;
            if (name == "min-resolution") return options.dpRatio >= dppx;
            if (name == "max-resolution") return options.dpRatio <= dppx;
            return std::fabs(options.dpRatio - dppx) < 0.001;
        }
        if (name == "hover" || name == "any-hover")
            return value == (options.touchDevice ? "none" : "hover");
        if (name == "pointer" || name == "any-pointer")
            return value == (options.touchDevice ? "coarse" : "fine");
        return false;
    }

    // one media query: [not|only] [type] [and (f) and (f) ...]
    bool evaluateQuery(const std::string& query, const CompatOptions& options){
        std::string text = trim(toLower(query));
        bool negate = false;
        if (startsWith(text, "not ")){
            negate = true;
            text = trim(text.substr(4));
        }else if (startsWith(text, "only ")){
            text = trim(text.substr(5));
        }
        bool result = true;
        size_t i = 0;
        while (i < text.size()){
            while (i < text.size() && text[i] == ' ')
                i++;
            if (i >= text.size())
                break;
            if (text[i] == '('){
                int depth = 0;
                size_t j = i;
                for (; j < text.size(); j++){
                    if (text[j] == '(') depth++;
                    else if (text[j] == ')' && --depth == 0) break;
                }
                result = result && evaluateFeature(text.substr(i + 1, j - i - 1), options);
                i = j + 1;
                continue;
            }
            size_t j = i;
            while (j < text.size() && text[j] != ' ' && text[j] != '(')
                j++;
            const std::string word = text.substr(i, j - i);
            if (word != "and" && word != "all" && word != "screen")
                result = false;   // print, speech, unknown types
            i = j;
        }
        return negate ? !result : result;
    }

    bool evaluateMedia(const std::string& media, const CompatOptions& options){
        if (trim(media).empty())
            return true;
        size_t start = 0;
        int depth = 0;
        for (size_t i = 0; i <= media.size(); i++){
            if (i < media.size() && media[i] == '(') depth++;
            if (i < media.size() && media[i] == ')') depth--;
            if (i == media.size() || (media[i] == ',' && depth == 0)){
                if (evaluateQuery(media.substr(start, i - start), options))
                    return true;
                start = i + 1;
            }
        }
        return false;
    }

    // ---- srcset ----

    struct Candidate {
        std::string url;
        double density = 0.0;   // x
        double width = 0.0;     // w
    };

    std::vector<Candidate> parseSrcset(const std::string& srcset){
        std::vector<Candidate> candidates;
        size_t i = 0;
        while (i < srcset.size()){
            while (i < srcset.size() && (srcset[i] == ' ' || srcset[i] == '\t' || srcset[i] == '\n' || srcset[i] == '\r' || srcset[i] == ','))
                i++;
            const size_t urlStart = i;
            while (i < srcset.size() && !(srcset[i] == ' ' || srcset[i] == '\t' || srcset[i] == '\n' || srcset[i] == '\r'))
                i++;
            std::string url = srcset.substr(urlStart, i - urlStart);
            bool ended = false;
            while (!url.empty() && url.back() == ','){
                url.pop_back();
                ended = true;
            }
            if (url.empty())
                continue;
            Candidate candidate;
            candidate.url = url;
            if (!ended){
                const size_t descriptorEnd = srcset.find(',', i);
                const std::string descriptors = srcset.substr(i, descriptorEnd == std::string::npos ? std::string::npos : descriptorEnd - i);
                for (const std::string& descriptor : splitWhitespace(descriptors)){
                    if (descriptor.empty())
                        continue;
                    const char unit = (char)std::tolower((unsigned char)descriptor.back());
                    const double value = std::strtod(descriptor.c_str(), nullptr);
                    if (unit == 'x')
                        candidate.density = value;
                    else if (unit == 'w')
                        candidate.width = value;
                }
                i = (descriptorEnd == std::string::npos) ? srcset.size() : descriptorEnd + 1;
            }
            if (candidate.density <= 0.0 && candidate.width <= 0.0)
                candidate.density = 1.0;
            candidates.push_back(candidate);
        }
        return candidates;
    }

    // sizes="": the first condition that matches, else the last length; 100vw without one
    double evaluateSizes(const std::string& sizes, const CompatOptions& options){
        viewportUsed = true;   // the slot size of a w descriptor is the viewport's at least
        if (trim(sizes).empty() || equalsIgnoreCase(trim(sizes), "auto"))
            return options.viewport.x;
        size_t start = 0;
        int depth = 0;
        std::vector<std::string> entries;
        for (size_t i = 0; i <= sizes.size(); i++){
            if (i < sizes.size() && sizes[i] == '(') depth++;
            if (i < sizes.size() && sizes[i] == ')') depth--;
            if (i == sizes.size() || (sizes[i] == ',' && depth == 0)){
                entries.push_back(trim(sizes.substr(start, i - start)));
                start = i + 1;
            }
        }
        for (const std::string& entry : entries){
            // the length is the last token; the rest a media condition
            const size_t space = entry.find_last_of(" )");
            std::string condition = (space == std::string::npos) ? std::string() : trim(entry.substr(0, space + 1));
            std::string length = (space == std::string::npos) ? entry : trim(entry.substr(space + 1));
            bool ok = false;
            const double px = lengthToPx(length, options, ok);
            if (!ok)
                continue;
            if (condition.empty() || evaluateMedia(condition, options))
                return px;
        }
        return options.viewport.x;
    }

    const Candidate* chooseCandidate(const std::vector<Candidate>& candidates, const std::string& sizes, const CompatOptions& options){
        if (candidates.empty())
            return nullptr;
        const double ratio = options.dpRatio > 0.0f ? options.dpRatio : 1.0;
        bool widths = false;
        for (const Candidate& candidate : candidates){
            if (candidate.width > 0.0)
                widths = true;
        }
        const double slot = widths ? evaluateSizes(sizes, options) : 0.0;
        // the effective density of each candidate
        const Candidate* best = nullptr;
        double bestDensity = 0.0;
        const Candidate* largest = nullptr;
        double largestDensity = 0.0;
        for (const Candidate& candidate : candidates){
            const double density = (candidate.width > 0.0 && slot > 0.0) ? candidate.width / slot : candidate.density;
            if (density >= ratio - 1e-6 && (!best || density < bestDensity)){
                best = &candidate;
                bestDensity = density;
            }
            if (!largest || density > largestDensity){
                largest = &candidate;
                largestDensity = density;
            }
        }
        return best ? best : largest;
    }

    std::string urlPathLower(const std::string& url){
        std::string path = url;
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string::npos)
            path = path.substr(0, cut);
        return toLower(path);
    }

    bool isDecodableType(const std::string& type){
        const std::string lower = toLower(trim(type));
        return lower.empty() || lower == "image/png" || lower == "image/jpeg" || lower == "image/jpg" || lower == "image/gif" ||
               lower == "image/bmp" || lower == "image/tga" || lower == "image/x-tga" || lower == "image/svg+xml";
    }

    // the local file a URL becomes, as far as the format matters ("" when unknown)
    std::string effectivePath(const TransformContext& context, const std::string& url){
        const UrlKind kind = UrlResolver::classify(url);
        if (kind == UrlKind::Remote){
            if (const ImportEntry* entry = context.urls.manifest().find(url))
                return toLower(entry->path);
            return urlPathLower(url);
        }
        if (kind == UrlKind::Data){
            const std::string lower = toLower(url.substr(0, std::min<size_t>(url.size(), 40)));
            if (lower.find("image/svg+xml") != std::string::npos) return "data.svg";
            if (lower.find("image/webp") != std::string::npos) return "data.webp";
            if (lower.find("image/avif") != std::string::npos) return "data.avif";
            return "data";
        }
        return urlPathLower(url);
    }

    bool isSvgSource(const TransformContext& context, const std::string& url){
        return endsWith(effectivePath(context, url), ".svg");
    }

    // ---- element rewrites ----

    // the attributes a replacement keeps from the original element
    void copyAttributes(const HtmlNode& from, HtmlNode& to, std::initializer_list<const char*> names){
        for (const char* name : names){
            if (const HtmlAttr* attribute = from.findAttr(name))
                to.attrs.push_back(*attribute);
        }
        for (const HtmlAttr& attribute : from.attrs){
            if (startsWith(attribute.name, "data-") || startsWith(attribute.name, "aria-") || attribute.name == "role"){
                if (!to.findAttr(attribute.name))
                    to.attrs.push_back(attribute);
            }
        }
    }

    std::unique_ptr<HtmlNode> makeReplacement(const HtmlNode& original, const std::string& tag){
        std::unique_ptr<HtmlNode> replacement = HtmlNode::makeElement(tag);
        replacement->implied = original.implied;
        replacement->ref = original.ref;
        return replacement;
    }

    void addSizeHints(TransformContext& context, HtmlNode& element, const HtmlNode& original){
        HintDeclarations hints;
        for (const char* name : {"width", "height"}){
            const HtmlAttr* attribute = original.findAttr(name);
            if (!attribute)
                continue;
            double number = 0.0;
            bool percent = false;
            if (!parseDimension(attribute->value, number, percent))
                continue;
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%s: %g%s; ", name, number, percent ? "%" : "px");
            hints.own += buffer;
            if (!hints.ref.valid())
                hints.ref = attribute->nameRef;
        }
        if (!hints.own.empty()){
            hints.own.pop_back();
            context.addHints(element, hints);
        }
    }

    void placeholder(TransformContext& context, HtmlNode& node, size_t index, const ElementSupport* row){
        HtmlNode& original = *node.children[index];
        std::unique_ptr<HtmlNode> div = makeReplacement(original, "div");
        copyAttributes(original, *div, {"id", "class", "style"});
        addClass(*div, "htmlui-unsupported");
        div->setAttr("data-html-tag", original.tag);
        addSizeHints(context, *div, original);
        const std::string suggestion = (row && row->suggestion) ? row->suggestion : "";
        context.sink.report(codes::UnsupportedElement, original.ref,
                            "<" + original.tag + "> cannot be drawn by the engine; a placeholder box of its default size is shown",
                            suggestion, "", original.tag);
        div->parent = &node;
        node.children[index] = std::move(div);
    }

    void mapLottie(TransformContext& context, HtmlNode& node, size_t index){
        HtmlNode& original = *node.children[index];
        std::unique_ptr<HtmlNode> lottie = makeReplacement(original, "lottie");
        std::string droppedNames;
        std::string background;
        for (const HtmlAttr& attribute : original.attrs){
            const std::string& name = attribute.name;
            if (name == "id" || name == "class" || name == "style" || name == "src" || name == "autoplay" || name == "speed" ||
                startsWith(name, "data-") || startsWith(name, "aria-") || name == "role" || name == "width" || name == "height"){
                if (name == "data-src" && !original.hasAttr("src")){
                    HtmlAttr src = attribute;
                    src.name = "src";
                    lottie->attrs.push_back(src);
                    continue;
                }
                lottie->attrs.push_back(attribute);
            }else if (name == "loop"){
                const std::string value = toLower(trim(attribute.value));
                if (value != "false"){
                    HtmlAttr loop = attribute;
                    loop.value = "true";
                    loop.hasValue = true;
                    if (!lottie->findAttr("loop"))
                        lottie->attrs.push_back(loop);
                }
            }else if (name == "count"){
                HtmlAttr loop = attribute;
                loop.name = "loop";
                lottie->removeAttr("loop");
                lottie->attrs.push_back(loop);
            }else if (name == "direction"){
                HtmlAttr direction = attribute;
                direction.value = (trim(attribute.value) == "-1") ? "-1" : "1";
                lottie->attrs.push_back(direction);
            }else if (name == "mode"){
                HtmlAttr mode = attribute;
                mode.value = equalsIgnoreCase(trim(attribute.value), "bounce") ? "bounce" : "normal";
                lottie->attrs.push_back(mode);
            }else if (name == "hover"){
                HtmlAttr hover = attribute;
                hover.name = "playonhover";
                hover.value = "";
                lottie->attrs.push_back(hover);
            }else if (name == "background"){
                background = trim(attribute.value);
            }else{
                droppedNames += (droppedNames.empty() ? "" : ", ") + name;
            }
        }
        if (!background.empty() && !equalsIgnoreCase(background, "transparent")){
            HtmlAttr* style = lottie->findAttr("style");
            const std::string declaration = "background-color: " + background + ";";
            if (style){
                style->value = declaration + " " + style->value;
            }else{
                lottie->setAttr("style", declaration);
            }
        }
        context.sink.report(codes::ElementMapped, original.ref, "<" + original.tag + "> is mapped to the engine's <lottie> element", "", "", original.tag);
        if (!droppedNames.empty()){
            context.sink.reportAs(Severity::Info, codes::UnsupportedAttribute, original.ref,
                                  "<" + original.tag + "> attributes without a <lottie> equivalent dropped: " + droppedNames, "", "", original.tag);
        }
        lottie->parent = &node;
        node.children[index] = std::move(lottie);
    }

    void mapMeter(TransformContext& context, HtmlNode& meter){
        const double value = std::strtod(meter.attr("value").c_str(), nullptr);
        const double min = meter.hasAttr("min") ? std::strtod(meter.attr("min").c_str(), nullptr) : 0.0;
        const double max = meter.hasAttr("max") ? std::strtod(meter.attr("max").c_str(), nullptr) : 1.0;
        std::string dropped;
        for (const char* name : {"low", "high", "optimum"}){
            if (meter.removeAttr(name))
                dropped += std::string(dropped.empty() ? "" : ", ") + name;
        }
        meter.removeAttr("min");
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%g", std::max(0.0, value - min));
        meter.setAttr("value", buffer);
        std::snprintf(buffer, sizeof(buffer), "%g", max > min ? max - min : 1.0);
        meter.setAttr("max", buffer);
        meter.tag = "progress";
        context.sink.report(codes::ElementMapped, meter.ref,
                            "<meter> is drawn as <progress>" + (dropped.empty() ? std::string() : " (" + dropped + " ignored)"),
                            "color the bar with a class for low/high states", "", "meter");
    }

    void flattenOptgroups(TransformContext& context, HtmlNode& select){
        for (size_t i = 0; i < select.children.size(); i++){
            HtmlNode& child = *select.children[i];
            if (!child.is("optgroup"))
                continue;
            std::unique_ptr<HtmlNode> header = makeReplacement(child, "option");
            header->setAttr("disabled", "").hasValue = false;
            header->setAttr("class", "htmlui-optgroup");
            header->appendChild(HtmlNode::makeText(child.attr("label")));
            context.sink.report(codes::ElementMapped, child.ref,
                                "<optgroup> is flattened: its label becomes a disabled header option", "", "", "optgroup");
            std::vector<std::unique_ptr<HtmlNode>> options = std::move(child.children);
            child.children.clear();
            if (child.hasAttr("disabled")){
                for (std::unique_ptr<HtmlNode>& option : options){
                    if (option->is("option") && !option->hasAttr("disabled"))
                        option->setAttr("disabled", "").hasValue = false;
                }
            }
            select.removeChild(&child);
            size_t at = i;
            select.insertChild(at++, std::move(header));
            for (std::unique_ptr<HtmlNode>& option : options){
                if (option->kind == HtmlNode::Kind::Element)
                    select.insertChild(at++, std::move(option));
            }
            i = at - 1;
        }
    }

    std::string fontSize(const std::string& value){
        const std::string text = trim(value);
        if (text.empty())
            return "";
        int size = 0;
        if (text[0] == '+' || text[0] == '-')
            size = 3 + std::atoi(text.c_str());
        else
            size = std::atoi(text.c_str());
        if (size < 1) size = 1;
        if (size > 7) size = 7;
        static const char* const sizes[] = {"10px", "13px", "16px", "18px", "24px", "32px", "48px"};
        return sizes[size - 1];
    }

    std::string fontFace(const std::string& face){
        std::string out;
        size_t start = 0;
        for (size_t i = 0; i <= face.size(); i++){
            if (i == face.size() || face[i] == ','){
                std::string name = trim(face.substr(start, i - start));
                if (name.size() >= 2 && (name.front() == '"' || name.front() == '\'') && name.back() == name.front())
                    name = name.substr(1, name.size() - 2);
                if (!name.empty())
                    out += (out.empty() ? "" : ", ") + name;
                start = i + 1;
            }
        }
        return out;
    }

    void mapFont(TransformContext& context, HtmlNode& font){
        HintDeclarations hints;
        if (const HtmlAttr* color = font.findAttr("color")){
            const std::string hex = legacyColor(color->value);
            if (!hex.empty()){
                hints.own += "color: " + hex + "; --html-current-color: " + hex + "; ";
                hints.ref = color->nameRef;
            }
        }
        if (const HtmlAttr* face = font.findAttr("face")){
            const std::string family = fontFace(face->value);
            if (!family.empty()){
                hints.own += "font-family: " + family + "; ";
                if (!hints.ref.valid())
                    hints.ref = face->nameRef;
            }
        }
        if (const HtmlAttr* size = font.findAttr("size")){
            const std::string px = fontSize(size->value);
            if (!px.empty()){
                hints.own += "font-size: " + px + "; ";
                if (!hints.ref.valid())
                    hints.ref = size->nameRef;
            }
        }
        font.removeAttr("color");
        font.removeAttr("face");
        font.removeAttr("size");
        font.tag = "span";
        if (!hints.own.empty()){
            hints.own.pop_back();
            context.addHints(font, hints);
        }
    }

    void mapDirection(TransformContext& context, HtmlNode& element){
        std::string direction = toLower(trim(element.attr("dir")));
        if (element.tag == "bdi" && direction.empty())
            direction = "auto";
        element.tag = "span";
        if (direction == "rtl" || direction == "ltr" || direction == "auto"){
            HintDeclarations hints;
            hints.own = "-rmlui-direction: " + direction + ";";
            if (const HtmlAttr* dir = element.findAttr("dir"))
                hints.ref = dir->nameRef;
            context.addHints(element, hints);
            element.removeAttr("dir");   // already hinted
        }
    }

    int quoteDepth(const HtmlNode& node){
        int depth = 0;
        for (const HtmlNode* walk = node.parent; walk; walk = walk->parent){
            if (walk->is("q"))
                depth++;
        }
        return depth;
    }

    void addQuotes(HtmlNode& q){
        // Chromium's quotes for English: “ ” outside, ‘ ’ when nested
        const bool nested = quoteDepth(q) % 2 == 1;
        q.insertChild(0, HtmlNode::makeText(nested ? "\xE2\x80\x98" : "\xE2\x80\x9C"));
        q.appendChild(HtmlNode::makeText(nested ? "\xE2\x80\x99" : "\xE2\x80\x9D"));
    }

    void chooseImageSource(TransformContext& context, HtmlNode& img, const std::vector<Candidate>& candidates, const std::string& sizes, SourceRef ref){
        const Candidate* chosen = chooseCandidate(candidates, sizes, context.options);
        if (!chosen)
            return;
        img.setAttr("src", chosen->url).valueRef = SourceRef{};
        char buffer[64];
        if (chosen->width > 0.0)
            std::snprintf(buffer, sizeof(buffer), "%gw", chosen->width);
        else
            std::snprintf(buffer, sizeof(buffer), "%gx", chosen->density);
        char ratio[32];
        std::snprintf(ratio, sizeof(ratio), "%g", (double)context.options.dpRatio);
        context.sink.report(codes::ImageCandidateChosen, ref,
                            "image candidate '" + chosen->url + "' (" + buffer + ") chosen at load for a dp ratio of " + ratio,
                            "", "", chosen->url);
    }

    void mapPicture(TransformContext& context, HtmlNode& node, size_t index){
        HtmlNode& picture = *node.children[index];
        HtmlNode* img = nullptr;
        for (std::unique_ptr<HtmlNode>& child : picture.children){
            if (child->is("img")){
                img = child.get();
                break;
            }
        }
        if (!img){
            // nothing to draw (as in browsers): the sources go, any other content stays
            picture.unwrap();
            return;
        }
        for (std::unique_ptr<HtmlNode>& child : picture.children){
            if (!child->is("source") || !child->hasAttr("srcset"))
                continue;
            if (!isDecodableType(child->attr("type")))
                continue;
            if (!evaluateMedia(child->attr("media"), context.options))
                continue;
            chooseImageSource(context, *img, parseSrcset(child->attr("srcset")), child->attr("sizes"), child->ref);
            img->removeAttr("srcset");
            img->removeAttr("sizes");
            break;
        }
        std::unique_ptr<HtmlNode> owned = picture.removeChild(img);
        owned->parent = &node;
        node.children[index] = std::move(owned);
    }

    // img: srcset, SVG sources (<svg src>), formats the engine cannot decode
    void mapImage(TransformContext& context, HtmlNode& node, size_t index){
        HtmlNode& img = *node.children[index];
        if (img.hasAttr("srcset")){
            std::vector<Candidate> candidates = parseSrcset(img.attr("srcset"));
            if (img.hasAttr("src") && !trim(img.attr("src")).empty()){
                bool hasOne = false;
                for (const Candidate& candidate : candidates){
                    if (candidate.width <= 0.0 && candidate.density == 1.0)
                        hasOne = true;
                }
                if (!hasOne){
                    Candidate src;
                    src.url = trim(img.attr("src"));
                    src.density = 1.0;
                    candidates.push_back(src);
                }
            }
            const HtmlAttr* srcset = img.findAttr("srcset");
            chooseImageSource(context, img, candidates, img.attr("sizes"), srcset->nameRef.valid() ? srcset->nameRef : img.ref);
            img.removeAttr("srcset");
            img.removeAttr("sizes");
        }

        const std::string src = trim(img.attr("src"));
        if (src.empty())
            return;
        const std::string path = effectivePath(context, src);
        if (endsWith(path, ".webp") || endsWith(path, ".avif") || endsWith(path, ".heic") || endsWith(path, ".jxl")){
            context.sink.report(codes::UnsupportedImageFormat, img.findAttr("src")->valueRef.valid() ? img.findAttr("src")->valueRef : img.ref,
                                "image format of '" + src + "' cannot be decoded by the engine; the image is not drawn",
                                "convert it to PNG (or JPEG)", "", src);
            return;
        }
        if (isSvgSource(context, src)){
            std::unique_ptr<HtmlNode> svg = makeReplacement(img, "svg");
            svg->ns = Namespace::Html;   // the engine element (RmlUi's SVG plugin), not inline SVG
            copyAttributes(img, *svg, {"id", "class", "style", "width", "height", "src"});
            svg->parent = &node;
            node.children[index] = std::move(svg);
        }
    }

    void transformSvg(TransformContext& context, HtmlNode& svg){
        bool reported = false;
        std::vector<HtmlNode*> stack{&svg};
        while (!stack.empty()){
            HtmlNode* current = stack.back();
            stack.pop_back();
            for (std::unique_ptr<HtmlNode>& child : current->children){
                if (child->kind != HtmlNode::Kind::Element)
                    continue;
                if (child->tag == "svg"){
                    // RmlUi reads an inline <svg> as raw text up to the first </svg>: a nested one
                    // would cut it short. A group keeps its content (its own viewport is lost)
                    context.sink.report(codes::SvgUnsupportedContent, child->ref,
                                        "an <svg> nested inside an inline SVG is drawn as a <g> (its viewBox, size and position are ignored)",
                                        "use <g transform=\"...\"> or <symbol>/<use>", "", "svg");
                    child->tag = "g";
                    for (const char* name : {"x", "y", "width", "height", "viewBox", "preserveAspectRatio", "xmlns"})
                        child->removeAttr(name);
                }
                if (child->tag == "foreignObject"){
                    if (!child->children.empty()){
                        if (!reported){
                            context.sink.report(codes::SvgUnsupportedContent, child->ref,
                                                "<foreignObject> content inside an SVG is not drawn (the SVG renderer has no HTML)",
                                                "place the HTML over the SVG with CSS positioning", "", "foreignObject");
                            reported = true;
                        }
                        child->children.clear();
                    }
                    continue;
                }
                stack.push_back(child.get());
            }
        }
    }

    bool isCustomElementName(const std::string& tag){
        return tag.find('-') != std::string::npos;
    }

    void transformChildren(TransformContext& context, HtmlNode& node);

    // one element; returns false when it was replaced or removed (the caller re-reads index)
    bool transformElement(TransformContext& context, HtmlNode& node, size_t& index){
        HtmlNode& element = *node.children[index];

        if (element.ns == Namespace::Svg){
            if (element.tag == "svg")
                transformSvg(context, element);
            return true;   // SVG content is the SVG renderer's
        }
        if (element.ns == Namespace::MathMl){
            if (element.tag == "math"){
                placeholder(context, node, index, findElement("math"));
                return false;
            }
            return true;
        }

        const std::string& tag = element.tag;
        // compat's own names (template placeholders, reserved renames, mapped elements, the
        // engine's <svg src> and <lottie>)
        if (tag == "lottie" && element.selfClosingSyntax && !element.children.empty()){
            // the engine's <lottie/> has no content: what the parser put inside goes after it
            std::vector<std::unique_ptr<HtmlNode>> moved = std::move(element.children);
            element.children.clear();
            size_t at = index + 1;
            for (std::unique_ptr<HtmlNode>& child : moved)
                node.insertChild(at++, std::move(child));
        }
        if (tag == "htmltemplate" || startsWith(tag, "html-reserved-") || tag == "lottie" || tag == "svg")
            return true;

        if (tag == "video" || tag == "audio" || tag == "iframe" || tag == "canvas" || tag == "object" || tag == "embed" ||
            tag == "portal" || tag == "frameset" || tag == "frame" || tag == "applet" || tag == "marquee"){
            placeholder(context, node, index, findElement(tag));
            return false;
        }
        if (tag == "track" || tag == "param" || tag == "map" || tag == "area"){
            const ElementSupport* row = findElement(tag);
            context.sink.report(codes::UnsupportedElement, element.ref, "<" + tag + "> is not supported and is removed",
                                (row && row->suggestion) ? row->suggestion : "", "", tag);
            node.removeChild(&element);
            return false;
        }
        if (tag == "datalist" || tag == "selectedcontent"){
            const ElementSupport* row = findElement(tag);
            context.sink.report(codes::ElementMapped, element.ref, "<" + tag + "> is removed (no engine equivalent)",
                                (row && row->suggestion) ? row->suggestion : "", "", tag);
            node.removeChild(&element);
            return false;
        }
        if (tag == "lottie-player" || tag == "dotlottie-player" || tag == "dotlottie-wc"){
            mapLottie(context, node, index);
            return false;
        }
        if (tag == "picture"){
            context.sink.report(codes::ElementMapped, element.ref, "<picture> is replaced by its <img> with the source chosen at load", "", "", "picture");
            mapPicture(context, node, index);
            return false;
        }
        if (tag == "img"){
            HtmlNode* before = node.children[index].get();
            mapImage(context, node, index);
            return node.children[index].get() == before;
        }
        if (tag == "wbr"){
            std::unique_ptr<HtmlNode> text = HtmlNode::makeText("\xE2\x80\x8B");
            text->parent = &node;
            node.children[index] = std::move(text);
            return false;
        }
        if (tag == "slot"){
            // no shadow DOM: the children are rendered (display: contents)
            element.unwrap();
            return false;
        }
        if (tag == "meter"){
            mapMeter(context, element);
            return true;
        }
        if (tag == "output"){
            element.tag = "span";
            context.sink.report(codes::ElementMapped, element.ref, "<output> is written as <span>", "", "", "output");
            return true;
        }
        if (tag == "select"){
            flattenOptgroups(context, element);
            return true;
        }
        if (tag == "optgroup"){
            // outside a select: its options stay where they are
            element.unwrap();
            return false;
        }
        if (tag == "q"){
            addQuotes(element);
            return true;
        }
        if (tag == "font"){
            mapFont(context, element);
            return true;
        }
        if (tag == "bdi" || tag == "bdo"){
            mapDirection(context, element);
            return true;
        }
        if (tag == "ruby" || tag == "rt" || tag == "rp"){
            if (!context.reportedRuby){
                context.sink.report(codes::ElementMapped, element.ref,
                                    "ruby annotations are laid out inline after their base at half size (no ruby layout); <rp> is hidden",
                                    "", "", "ruby");
                context.reportedRuby = true;
            }
            return true;
        }
        if (tag == "dialog"){
            if (!context.reportedDialog){
                context.sink.report(codes::DialogNotModal, element.ref,
                                    "dialogs have no modal backdrop or focus trap (showModal opens it like show)",
                                    "cover the screen with a positioned element behind the dialog", "", "dialog");
                context.reportedDialog = true;
            }
            return true;
        }

        if (findElement(tag) == nullptr){
            if (isCustomElementName(tag)){
                context.sink.report(codes::UnknownCustomElement, element.ref,
                                    "custom element <" + tag + "> is rendered as a generic inline element (its JavaScript class does not run)",
                                    "style it with CSS (display: block, ...)", "", tag);
            }else{
                context.sink.report(codes::UnknownCustomElement, element.ref,
                                    "unknown element <" + tag + "> is rendered as a generic inline element, as in browsers",
                                    "use a standard element (div, span, ...)", "", tag);
            }
        }
        return true;
    }

    void transformChildren(TransformContext& context, HtmlNode& node){
        for (size_t i = 0; i < node.children.size();){
            HtmlNode* child = node.children[i].get();
            if (child->kind != HtmlNode::Kind::Element){
                i++;
                continue;
            }
            if (!transformElement(context, node, i))
                continue;   // the slot holds a new node (or the next one): look at it again
            if (child->ns == Namespace::Html)
                transformChildren(context, *child);
            i++;
        }
    }

}

void jasperengine::htmlui::compat::transformElements(TransformContext& context, HtmlNode& tree){
    viewportUsed = false;
    transformChildren(context, tree);
    if (viewportUsed)
        context.dependsOnViewport = true;
}
