// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIAssertions.h"
#include "HtmlUIInspectorInternal.h"

#include "Scene.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/compat/LuaHooks.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/FontMetrics.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_set>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::htmlui;
using namespace jasperengine::htmlui::inspect;

namespace {

    std::string px(float value){
        return number(value, 1) + "px";
    }

    std::string rectText(const InspectRect& rect){
        return "[" + number(rect.x, 1) + "," + number(rect.y, 1) + " " + number(rect.width, 1) + "x" + number(rect.height, 1) + "]";
    }

    // how far `inner` reaches past each edge of `outer`, beyond tolerance: "12px right, 4px bottom"
    std::string overflowText(const InspectRect& outer, const InspectRect& inner, float tolerance){
        std::string text;
        const auto add = [&text, tolerance](float over, const char* edge){
            if (over > tolerance)
                text += (text.empty() ? "" : ", ") + px(over) + " " + edge;
        };
        add(outer.y - inner.y, "top");
        add(inner.x + inner.width - (outer.x + outer.width), "right");
        add(inner.y + inner.height - (outer.y + outer.height), "bottom");
        add(outer.x - inner.x, "left");
        return text;
    }

    bool isScroller(Rml::Element* element, bool horizontal){
        const Rml::Style::ComputedValues& computed = element->GetComputedValues();
        const Rml::Style::Overflow overflow = horizontal ? computed.overflow_x() : computed.overflow_y();
        return overflow == Rml::Style::Overflow::Auto || overflow == Rml::Style::Overflow::Scroll;
    }

    // hidden for the checks: display: none (it or an ancestor) or visibility: hidden
    bool isHidden(Rml::Element* element){
        return displayNoneAt(element) || element->GetComputedValues().visibility() != Rml::Style::Visibility::Visible;
    }

    // what NoOverlap and elementRect call visible: shown, opaque enough, not zero-size
    bool isShown(const Doc& doc, Rml::Element* element){
        const InspectFlags flags = flagsOf(doc, element);
        return !flags.hidden && !flags.transparent && !flags.zeroSize;
    }

    // the matches and their element descendants, document order, each once; owner = the match
    // it was reached from (the outermost)
    void withDescendants(const std::vector<Rml::Element*>& matches, std::vector<std::pair<Rml::Element*, Rml::Element*>>& out){
        out.clear();
        std::unordered_set<Rml::Element*> seen;
        std::vector<Rml::Element*> children;
        for (Rml::Element* match : matches){
            std::vector<Rml::Element*> stack{match};
            while (!stack.empty()){
                Rml::Element* element = stack.back();
                stack.pop_back();
                if (!seen.insert(element).second)
                    continue;
                out.emplace_back(element, match);
                elementChildren(element, children);
                for (auto it = children.rbegin(); it != children.rend(); ++it)
                    stack.push_back(*it);
            }
        }
    }

    // ---- text lines ----

    // the untransformed layout box of an area (TextFits compares text and boxes in layout space)
    InspectRect layoutRect(Rml::Element* element, Rml::BoxArea area){
        const Rml::Vector2f position = element->GetAbsoluteOffset(area);
        const Rml::Vector2f size = element->GetBox().GetSize(area);
        return InspectRect{position.x, position.y, size.x, size.y};
    }

    bool isSpace(char c){
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }

    // The line boxes of an element's direct text: each line's text measured as the font engine
    // lays it out (the widths RmlUi keeps are only filled when it draws), from the baseline
    // RmlUi places it at, as tall as the line height, with the space a justified line adds.
    // Collapsible white space at the ends of a line hangs and is left out.
    std::vector<InspectRect> textLines(Rml::Element* element){
        std::vector<InspectRect> lines;
        if (isLeaf(element))
            return lines;
        const int count = element->GetNumChildren();
        for (int i = 0; i < count; i++){
            Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element->GetChild(i));
            if (!text)
                continue;
            const Rml::FontFaceHandle font = text->GetFontFaceHandle();
            if (!font)
                continue;
            // the line box above the baseline as the layout places it (InlineBoxBase::GetStrut, as
            // Chromium: whole-pixel ascent and descent, the half-leading rounded down)
            const Rml::FontMetrics& metrics = Rml::GetFontEngineInterface()->GetFontMetrics(font);
            const float lineHeight = text->GetLineHeight();
            float ascent = std::floor(metrics.ascent + 0.5f);
            float descent = std::floor(metrics.descent + 0.5f);
            if (descent < metrics.descent && ascent >= 1.0f){
                descent += 1.0f;
                ascent -= 1.0f;
            }
            const float top = ascent + std::floor(0.5f * (lineHeight - (ascent + descent)));
            const Rml::Vector2f origin = text->GetAbsoluteOffset(Rml::BoxArea::Border);
            for (const Rml::ElementText::Line& line : text->GetLines()){
                const std::string& content = line.text;
                size_t begin = 0, end = content.size();
                while (begin < end && isSpace(content[begin]))
                    begin++;
                while (end > begin && isSpace(content[end - 1]))
                    end--;
                if (begin == end)
                    continue;
                float leading = begin > 0 ? (float)Rml::ElementUtilities::GetStringWidth(text, Rml::StringView(content.data(), content.data() + begin)) : 0.0f;
                float width = (float)Rml::ElementUtilities::GetStringWidth(text, Rml::StringView(content.data() + begin, content.data() + end));
                // a justified line: the space the layout added around its characters
                for (const Rml::JustificationSpacing& spacing : line.justification){
                    const float added = spacing.before + spacing.after;
                    if (spacing.offset < (int)begin)
                        leading += added;
                    else if (spacing.offset < (int)end)
                        width += added;
                }
                const float baseline = origin.y + line.position.y;
                lines.push_back(InspectRect{origin.x + line.position.x + leading, baseline - top, width, lineHeight});
            }
        }
        return lines;
    }

    // ---- contrast ----

    struct Rgba { double r = 0, g = 0, b = 0, a = 0; };   // premultiplied, 0-1

    Rgba premultiplied(const Rml::Colourb& colour, double alphaScale = 1.0){
        const double a = colour.alpha / 255.0 * alphaScale;
        return Rgba{colour.red / 255.0 * a, colour.green / 255.0 * a, colour.blue / 255.0 * a, a};
    }

    Rgba over(const Rgba& top, const Rgba& bottom){
        const double k = 1.0 - top.a;
        return Rgba{top.r + bottom.r * k, top.g + bottom.g * k, top.b + bottom.b * k, top.a + bottom.a * k};
    }

    Rgba scaled(const Rgba& colour, double factor){
        return Rgba{colour.r * factor, colour.g * factor, colour.b * factor, colour.a * factor};
    }

    double linear(double channel){
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    }

    double luminance(const Rgba& colour){
        return 0.2126 * linear(colour.r) + 0.7152 * linear(colour.g) + 0.0722 * linear(colour.b);
    }

    std::string hex(const Rgba& colour){
        const auto byte = [](double value){ return (int)std::lround(std::min(1.0, std::max(0.0, value)) * 255.0); };
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", byte(colour.r), byte(colour.g), byte(colour.b));
        return buffer;
    }

    // One element's text over what is drawn below it: the element and its ancestors, each a
    // group (background colour, background image, then its content) composited with its
    // opacity, as the renderer does. Unknowns (an image background, the transparent root)
    // are found by computing with two extremes: when the result changes, it is inconclusive.
    struct ContrastStack {
        struct Layer {
            Rml::Colourb background;
            bool image = false;
            float opacity = 1.0f;
        };
        std::vector<Layer> layers;   // the element first, the root last
        Rml::Colourb text;

        explicit ContrastStack(Rml::Element* element){
            text = element->GetComputedValues().color();
            for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
                const Rml::Style::ComputedValues& computed = walk->GetComputedValues();
                layers.push_back(Layer{computed.background_color(), computed.has_decorator() || computed.has_background_image(), computed.opacity()});
                if (rmlui_dynamic_cast<Rml::ElementDocument*>(walk))
                    break;
            }
        }

        // imageMode 0: images drawn as nothing, 1 opaque black, 2 opaque white
        Rgba compose(bool withText, int imageMode, const Rgba& backdrop) const{
            Rgba group;
            for (size_t i = 0; i < layers.size(); i++){
                const Layer& layer = layers[i];
                Rgba content = premultiplied(layer.background);
                if (layer.image && imageMode > 0)
                    content = over(imageMode == 1 ? Rgba{0, 0, 0, 1} : Rgba{1, 1, 1, 1}, content);
                if (i == 0){
                    if (withText)
                        content = over(premultiplied(text), content);
                }else{
                    content = over(scaled(group, layers[i - 1].opacity), content);
                }
                group = content;
            }
            return over(scaled(group, layers.back().opacity), backdrop);
        }
    };

    bool same(const Rgba& a, const Rgba& b){
        return std::fabs(a.r - b.r) < 1e-4 && std::fabs(a.g - b.g) < 1e-4 && std::fabs(a.b - b.b) < 1e-4;
    }

    // ---- the checks ----

    class Checker {
    public:
        Checker(const Doc& doc, const AssertSpec& spec, AssertResult& result): doc(doc), spec(spec), result(result), refs(doc) {}

        void fail(Rml::Element* element, const std::string& message){
            AssertFailure failure;
            failure.ref = refs.of(element);
            failure.message = message;
            failure.rect = borderRect(element);
            result.failures.push_back(std::move(failure));
        }

        void fail(Rml::Element* element, const std::string& message, Rml::Element* other, const InspectRect& otherRect){
            fail(element, message);
            result.failures.back().otherRef = refs.of(other);
            result.failures.back().otherRect = otherRect;
        }

        void count(size_t matched){
            const int n = (int)matched;
            const int min = spec.kind == AssertKind::Exists && spec.min < 0 ? 1 : spec.min;
            std::string message;
            if (spec.count >= 0 && n != spec.count)
                message = "matched " + std::to_string(n) + ", expected exactly " + std::to_string(spec.count);
            else if (min >= 0 && n < min)
                message = "matched " + std::to_string(n) + ", expected at least " + std::to_string(min);
            else if (spec.max >= 0 && n > spec.max)
                message = "matched " + std::to_string(n) + ", expected at most " + std::to_string(spec.max);
            if (!message.empty()){
                AssertFailure failure;
                failure.ref = spec.selector;
                failure.message = message;
                result.failures.push_back(std::move(failure));
            }
        }

        void visible(const std::vector<Rml::Element*>& matches){
            const InspectRect viewport{0, 0, doc.width, doc.height};
            for (Rml::Element* element : matches){
                if (Rml::Element* none = displayNoneAt(element)){
                    fail(element, "display: none" + (none == element ? std::string() : " on " + refs.of(none)));
                    continue;
                }
                if (element->GetComputedValues().visibility() != Rml::Style::Visibility::Visible){
                    fail(element, "visibility: hidden");
                    continue;
                }
                const float opacity = effectiveOpacity(element);
                if (opacity < 0.05f){
                    fail(element, "effective opacity " + number(opacity) + " (below 0.05)");
                    continue;
                }
                const InspectRect border = borderRect(element);
                if (border.width < 1.0f || border.height < 1.0f){
                    fail(element, "border box is " + px(border.width) + " x " + px(border.height));
                    continue;
                }
                InspectRect part = intersect(border, viewport);
                if (isEmpty(part)){
                    fail(element, "border box " + rectText(border) + " is outside the " + number(doc.width) + "x" + number(doc.height) + " viewport");
                    continue;
                }
                const ClipRegion clip = clipOf(element);
                if (clip.bounded){
                    part = intersect(part, clip.rect);
                    if (isEmpty(part)){
                        Rml::Element* clipper = clipperCutting(clip, border, 0.0f);
                        fail(element, "fully clipped by the overflow of " + (clipper ? refs.of(clipper) : std::string("an ancestor")));
                        continue;
                    }
                }
                if (spec.unobstructed)
                    unobstructed(element, part);
            }
        }

        void unobstructed(Rml::Element* element, const InspectRect& part){
            const float x = part.x + part.width * 0.5f, y = part.y + part.height * 0.5f;
            const std::string at = " at (" + number(x, 1) + ", " + number(y, 1) + ")";
            if (element->GetComputedValues().pointer_events() == Rml::Style::PointerEvents::None){
                fail(element, "pointer-events: none: the pointer goes through it" + at);
                return;
            }
            // the context's topmost element: an element of another document over this one covers it
            // (a transparent page does not: the pointer passes through it)
            Rml::Element* hit = hitAt(doc, x, y, true);
            if (!hit){
                fail(element, "the pointer falls through to the game" + at);
                return;
            }
            Rml::ElementDocument* hitDocument = hit->GetOwnerDocument();
            if (hitDocument != doc.document){
                fail(element, "covered by " + otherDocumentRef(hit) + " of the UI document " + documentName(hitDocument) + at);
                return;
            }
            if (!isInclusiveAncestor(element, hit))
                fail(element, "covered by " + refs.of(hit) + at, hit, borderRect(hit));
        }

        // an element of another document, for messages: its id or tag
        static std::string otherDocumentRef(Rml::Element* element){
            if (!element->GetId().empty())
                return "#" + element->GetId();
            return "<" + compat::LuaHooks::localName(element) + ">";
        }

        // the asset path of another document of the scene, for messages
        std::string documentName(Rml::ElementDocument* document){
            for (Entity entity : HtmlUIInspector::documents(doc.scene)){
                if (doc.system->getDocument(entity) == document){
                    if (const UIDocumentComponent* component = doc.scene->findComponent<UIDocumentComponent>(entity))
                        return component->filename;
                }
            }
            return document ? document->GetSourceURL() : std::string();
        }

        void textFits(const std::vector<Rml::Element*>& matches){
            std::vector<std::pair<Rml::Element*, Rml::Element*>> elements;
            withDescendants(matches, elements);
            for (const auto& entry : elements){
                Rml::Element* element = entry.first;
                Rml::Element* match = entry.second;
                if (isHidden(element) || ownText(element).empty())
                    continue;
                const std::vector<InspectRect> lines = textLines(element);
                if (lines.empty())
                    continue;
                InspectRect text = lines.front();
                std::set<long> rows;
                for (const InspectRect& line : lines){
                    text = unite(text, line);
                    rows.insert(std::lround(line.y * 2.0f));
                }

                // inline text lays out in the content box of its block container
                Rml::Element* box = element;
                while (box->GetComputedValues().display() == Rml::Style::Display::Inline && box->GetParentNode() &&
                       !rmlui_dynamic_cast<Rml::ElementDocument*>(box))
                    box = box->GetParentNode();
                const InspectRect content = layoutRect(box, Rml::BoxArea::Content);

                // allow_scroll: a scroll container from the element up to the match takes the overflow
                bool scrollX = false, scrollY = false;
                if (spec.allowScroll){
                    for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
                        scrollX = scrollX || isScroller(walk, true);
                        scrollY = scrollY || isScroller(walk, false);
                        if (walk == match)
                            break;
                    }
                }

                std::vector<std::string> problems;
                const float overX = std::max(0.0f, content.x - text.x) + std::max(0.0f, text.x + text.width - (content.x + content.width));
                const float overY = std::max(0.0f, content.y - text.y) + std::max(0.0f, text.y + text.height - (content.y + content.height));
                if (overX > spec.tolerance && !scrollX)
                    problems.push_back("text is " + px(text.width) + " wide in a " + px(content.width) + " content box (" + px(overX) + " over)");
                if (overY > spec.tolerance && !scrollY)
                    problems.push_back("text is " + px(text.height) + " tall in a " + px(content.height) + " content box (" + px(overY) + " over)");

                // an ancestor's overflow cutting the text (a scroll container up to the match is
                // allowed with allow_scroll)
                const std::function<bool(Rml::Element*)> skip = [&](Rml::Element* clipping){
                    return spec.allowScroll && (isScroller(clipping, true) || isScroller(clipping, false)) &&
                           isInclusiveAncestor(match, clipping);
                };
                Rml::Element* firstText = nullptr;
                for (int i = 0; i < element->GetNumChildren() && !firstText; i++){
                    if (rmlui_dynamic_cast<Rml::ElementText*>(element->GetChild(i)))
                        firstText = element->GetChild(i);
                }
                const ClipRegion clip = clipOf(firstText ? firstText : element, skip);
                if (clip.bounded){
                    // the clip regions are boxes through their CSS transforms: the text goes through its own
                    // (a card centered with translateX(-50%) is not cut where its layout box would be)
                    const InspectRect shown = transformedRect(element, text);
                    const InspectRect visible = intersect(shown, clip.rect);
                    const float hiddenX = shown.width - visible.width;
                    const float hiddenY = shown.height - visible.height;
                    if (hiddenX > spec.tolerance || hiddenY > spec.tolerance){
                        Rml::Element* clipper = clipperCutting(clip, shown, spec.tolerance);
                        problems.push_back("text is cut by the overflow of " + (clipper ? refs.of(clipper) : std::string("an ancestor")) + " (" +
                                           px(hiddenX > spec.tolerance ? hiddenX : hiddenY) + " hidden)");
                    }
                }
                if (spec.singleLine && rows.size() > 1)
                    problems.push_back("text wraps to " + std::to_string(rows.size()) + " lines (single line expected)");

                if (!problems.empty()){
                    std::string message;
                    for (const std::string& problem : problems)
                        message += (message.empty() ? "" : "; ") + problem;
                    fail(element, message);
                }
            }
        }

        bool noOverlap(const std::vector<Rml::Element*>& a){
            std::vector<Rml::Element*> b;
            const bool pairwise = spec.with.empty();
            if (!pairwise){
                std::string error;
                if (!select(doc, spec.with, b, error)){
                    result.error = error;
                    return false;
                }
                if (b.empty()){
                    AssertFailure failure;
                    failure.ref = spec.with;
                    failure.message = "with selector matched nothing";
                    result.failures.push_back(std::move(failure));
                    return true;
                }
                std::unordered_set<Rml::Element*> all(a.begin(), a.end());
                for (Rml::Element* element : b)
                    all.insert(element);
                result.matched = all.size();
            }
            std::vector<Rml::Element*> left, right;
            for (Rml::Element* element : a){
                if (isShown(doc, element))
                    left.push_back(element);
            }
            for (Rml::Element* element : pairwise ? a : b){
                if (isShown(doc, element))
                    right.push_back(element);
            }
            std::set<std::pair<Rml::Element*, Rml::Element*>> done;
            for (size_t i = 0; i < left.size(); i++){
                for (size_t j = pairwise ? i + 1 : 0; j < right.size(); j++){
                    Rml::Element* first = left[i];
                    Rml::Element* second = right[j];
                    if (first == second || isInclusiveAncestor(first, second) || isInclusiveAncestor(second, first))
                        continue;
                    const auto key = first < second ? std::make_pair(first, second) : std::make_pair(second, first);
                    if (!done.insert(key).second)
                        continue;
                    const InspectRect firstRect = borderRect(first);
                    const InspectRect secondRect = borderRect(second);
                    const InspectRect overlap = intersect(firstRect, secondRect);
                    if (overlap.width > spec.tolerance && overlap.height > spec.tolerance)
                        fail(first, "overlaps " + refs.of(second) + " by " + px(overlap.width) + " x " + px(overlap.height) + " at " + rectText(overlap),
                             second, secondRect);
                }
            }
            return true;
        }

        bool withinParent(const std::vector<Rml::Element*>& matches){
            std::string container;
            if (!spec.container.empty()){
                std::string error;
                if (!rmlSelector(doc, spec.container, container, error)){
                    result.error = error;
                    return false;
                }
            }
            for (Rml::Element* element : matches){
                if (isHidden(element))
                    continue;
                Rml::Element* parent = nullptr;
                for (Rml::Element* walk = element->GetParentNode(); walk; walk = walk->GetParentNode()){
                    if (container.empty() || walk->Matches(container)){
                        parent = walk;
                        break;
                    }
                    if (rmlui_dynamic_cast<Rml::ElementDocument*>(walk))
                        break;
                }
                if (!parent || (container.empty() && !isElement(parent)) || !parent->GetOwnerDocument()){
                    if (!container.empty())
                        fail(element, "no ancestor matches \"" + spec.container + "\"");
                    continue;
                }
                const InspectRect padding = boxRect(parent, Rml::BoxArea::Padding);
                const InspectRect border = borderRect(element);
                const std::string over = overflowText(padding, border, spec.tolerance);
                if (!over.empty())
                    fail(element, "extends past the padding box of " + refs.of(parent) + ": " + over, parent, padding);
            }
            return true;
        }

        void withinViewport(const std::vector<Rml::Element*>& matches){
            const InspectRect viewport{0, 0, doc.width, doc.height};
            for (Rml::Element* element : matches){
                if (isHidden(element))
                    continue;
                const std::string over = overflowText(viewport, borderRect(element), spec.tolerance);
                if (!over.empty())
                    fail(element, "extends past the " + number(doc.width) + "x" + number(doc.height) + " viewport: " + over);
            }
        }

        void contrast(const std::vector<Rml::Element*>& matches){
            std::vector<std::pair<Rml::Element*, Rml::Element*>> elements;
            withDescendants(matches, elements);
            std::vector<Rml::Element*> checked;
            for (const auto& entry : elements){
                if (!ownText(entry.first).empty() && isShown(doc, entry.first))
                    checked.push_back(entry.first);
            }
            if (checked.empty()){
                for (Rml::Element* element : matches){
                    if (isShown(doc, element))
                        checked.push_back(element);
                }
            }
            const Rgba black{0, 0, 0, 1}, white{1, 1, 1, 1};
            const Rgba given{spec.background[0], spec.background[1], spec.background[2], 1};
            for (Rml::Element* element : checked){
                const ContrastStack stack(element);
                // the transparent root needs `background`; an image on the way makes it unknown
                const bool rootShows = !same(stack.compose(false, 0, black), stack.compose(false, 0, white)) ||
                                       !same(stack.compose(true, 0, black), stack.compose(true, 0, white));
                if (rootShows && !spec.hasBackground){
                    result.inconclusive.push_back(refs.of(element));
                    continue;
                }
                const Rgba backdrop = spec.hasBackground ? given : black;
                bool image = false;
                for (bool withText : {false, true}){
                    const Rgba plain = stack.compose(withText, 0, backdrop);
                    image = image || !same(plain, stack.compose(withText, 1, backdrop)) || !same(plain, stack.compose(withText, 2, backdrop));
                }
                if (image){
                    result.inconclusive.push_back(refs.of(element));
                    continue;
                }
                const Rgba background = stack.compose(false, 0, backdrop);
                const Rgba foreground = stack.compose(true, 0, backdrop);
                const double l1 = luminance(foreground), l2 = luminance(background);
                const double ratio = (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);

                const Rml::Style::ComputedValues& computed = element->GetComputedValues();
                const float size = computed.font_size();
                const bool large = size >= 24.0f || (size >= 18.66f && (int)computed.font_weight() >= 700);
                const double needed = spec.minRatio > 0 ? spec.minRatio : (large ? 3.0 : 4.5);
                if (ratio + 1e-6 < needed)
                    fail(element, "contrast " + number(ratio) + ":1 (" + hex(foreground) + " on " + hex(background) + ") is below " + number(needed) + ":1" +
                                  (spec.minRatio > 0 ? std::string() : large ? " (large text)" : std::string()));
            }
        }

        void minSize(const std::vector<Rml::Element*>& matches){
            for (Rml::Element* element : matches){
                if (isHidden(element) || effectiveOpacity(element) < 0.05f)
                    continue;
                const InspectRect border = borderRect(element);
                if (border.width + 1e-3f < spec.minWidth || border.height + 1e-3f < spec.minHeight)
                    fail(element, "is " + number(border.width, 1) + "x" + number(border.height, 1) + "px, smaller than " +
                                  number(spec.minWidth, 1) + "x" + number(spec.minHeight, 1) + "px");
            }
        }

    private:
        const Doc& doc;
        const AssertSpec& spec;
        AssertResult& result;
        Refs refs;
    };

}

AssertResult HtmlUIAssertions::run(Scene* scene, Entity entity, const AssertSpec& spec){
    AssertResult result;
    const auto error = [&result](const std::string& message){
        result.passed = false;
        result.error = message;
        result.failures.clear();
        return result;
    };

    Doc doc;
    std::string message;
    if (!open(scene, entity, doc, message))
        return error(message);
    if (spec.selector.find_first_not_of(" \t\r\n") == std::string::npos)
        return error("selector is empty");
    std::vector<Rml::Element*> matches;
    if (!select(doc, spec.selector, matches, message))
        return error(message);
    result.matched = matches.size();

    Checker checker(doc, spec, result);
    if (matches.empty() && !(spec.kind == AssertKind::Count && (spec.max == 0 || spec.count == 0))){
        AssertFailure failure;
        failure.ref = spec.selector;
        failure.message = "selector matched nothing";
        result.failures.push_back(std::move(failure));
        result.passed = false;
        return result;
    }

    switch (spec.kind){
        case AssertKind::Exists:
        case AssertKind::Count:
            checker.count(matches.size());
            break;
        case AssertKind::Visible:
            checker.visible(matches);
            break;
        case AssertKind::TextFits:
            checker.textFits(matches);
            break;
        case AssertKind::NoOverlap:
            if (!checker.noOverlap(matches))
                return error(result.error);
            break;
        case AssertKind::WithinParent:
            if (!checker.withinParent(matches))
                return error(result.error);
            break;
        case AssertKind::WithinViewport:
            checker.withinViewport(matches);
            break;
        case AssertKind::Contrast:
            checker.contrast(matches);
            break;
        case AssertKind::MinSize:
            checker.minSize(matches);
            break;
    }
    result.passed = result.error.empty() && result.failures.empty();
    return result;
}
