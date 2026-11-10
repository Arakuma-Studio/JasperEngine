// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Text splitting follows Anime.js 4.5.0 src/text/split.js (MIT). The fork's existing UAX #29
// segmenter supplies Intl.Segmenter('grapheme'); layout is synchronous instead of a browser
// ResizeObserver/debounce, so an exact frame capture never depends on a wall-clock timeout.
// Words use Anime's no-Intl fallback (whitespace groups); dictionary word segmentation is
// a browser Intl facility, independent of the extended-grapheme character splitting below.

#include "AnimText.h"

#include "AnimEngine.h"
#include "Log.h"
#include "object/ui/UIElementInternal.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>

#include "../../../libs/rmlui/Source/Core/TextJustification.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

using namespace jasperengine;

namespace {

    std::vector<std::shared_ptr<AnimText>>* animTextRegistered = nullptr;

    std::vector<std::shared_ptr<AnimText>>& animTextSplitters(){
        if (!animTextRegistered) animTextRegistered = new std::vector<std::shared_ptr<AnimText>>();
        return *animTextRegistered;
    }

    bool animTextEnabled(const AnimParam& params, const char* name, bool fallback){
        const AnimParam* value = params.find(name);
        return value ? value->isTruthy() : fallback;
    }

    bool animTextSpace(char32_t c){
        return c == 0x20 || (c >= 0x09 && c <= 0x0d) || c == 0xa0 || c == 0x1680 ||
               (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f ||
               c == 0x205f || c == 0x3000 || c == 0xfeff;
    }

    bool animTextWhitespace(const std::string& text){
        Rml::Vector<Rml::TextJustification::Cluster> clusters;
        Rml::TextJustification::GetClusters(text, clusters);
        return std::all_of(clusters.begin(), clusters.end(), [](const auto& c){ return animTextSpace((char32_t)c.first); });
    }

    Rml::ElementPtr animTextNode(Rml::Element* parent, const std::string& text){
        Rml::ElementPtr node = Rml::Factory::InstanceElement(parent, "#text", "#text", {});
        if (auto* element = rmlui_dynamic_cast<Rml::ElementText*>(node.get()))
            element->SetText(text);
        return node;
    }

    void animTextReadingText(Rml::Element* element, std::string& out, bool nested = false){
        if (const auto* text = rmlui_dynamic_cast<Rml::ElementText*>(element)){
            out += text->GetText();
            return;
        }
        if (element->GetTagName() == "br"){
            out += '\n';
            return;
        }
        const auto display = element->GetComputedValues().display();
        if (display == Rml::Style::Display::None || element->GetAttribute<std::string>("aria-hidden", "") == "true") return;
        const bool block = nested && (display == Rml::Style::Display::Block || display == Rml::Style::Display::FlowRoot ||
            display == Rml::Style::Display::Flex || display == Rml::Style::Display::Grid || display == Rml::Style::Display::Table ||
            display == Rml::Style::Display::TableRow || display == Rml::Style::Display::TableCell);
        if (block && !out.empty() && out.back() != '\n') out += '\n';
        for (int i = 0; i < element->GetNumChildren(); ++i) animTextReadingText(element->GetChild(i), out, true);
        if (block && !out.empty() && out.back() != '\n') out += '\n';
    }

    struct AnimTextWrapper {
        Rml::ElementPtr outer;
        Rml::Element* unit = nullptr;
    };

    AnimTextWrapper animTextWrap(Rml::Element* parent, const char* type, const AnimParam& options, int index,
                                int wordIndex = -1, int charIndex = -1){
        AnimTextWrapper result;
        Rml::ElementPtr unit = Rml::Factory::InstanceElement(parent, "span", "span", {});
        result.unit = unit.get();
        const std::string display = std::string(type) == "line" ? "block" : "inline-block";
        unit->SetProperty("display", display);
        unit->SetAttribute(std::string("data-") + type, std::to_string(index));
        unit->SetAttribute("data-line", "-1");
        if (wordIndex >= 0) unit->SetAttribute("data-word", std::to_string(wordIndex));
        if (charIndex >= 0) unit->SetAttribute("data-char", std::to_string(charIndex));
        const AnimParam& klass = options.get("class");
        if (klass.isString()) unit->SetClassNames(klass.asString());
        const AnimParam& wrap = options.get("wrap");
        if (wrap.isTruthy()){
            result.outer = Rml::Factory::InstanceElement(parent, "span", "span", {});
            result.outer->SetProperty("display", display);
            const std::string overflow = wrap.isString() && wrap.asString() == "visible" ? "visible" : "hidden";
            result.outer->SetProperty("overflow-x", overflow);
            result.outer->SetProperty("overflow-y", overflow);
            result.outer->AppendChild(std::move(unit));
        } else {
            result.outer = std::move(unit);
        }
        return result;
    }

    struct AnimTextSegment {
        std::string text;
        bool space;
    };

    std::vector<AnimTextSegment> animTextSegments(const std::string& text){
        Rml::Vector<Rml::TextJustification::Cluster> clusters;
        Rml::TextJustification::GetClusters(text, clusters);
        std::vector<AnimTextSegment> result;
        for (size_t i = 0; i < clusters.size(); ++i){
            const bool space = animTextSpace((char32_t)clusters[i].first);
            const size_t begin = (size_t)clusters[i].offset;
            const size_t end = i + 1 < clusters.size() ? (size_t)clusters[i + 1].offset : text.size();
            if (result.empty() || result.back().space != space)
                result.push_back({{}, space});
            result.back().text.append(text, begin, end - begin);
        }
        return result;
    }

    void animTextSplitNode(Rml::Element* parent, const AnimParam& params, bool needWords,
                          std::vector<UIElement>& words, std::vector<UIElement>& chars){
        const bool needChars = animTextEnabled(params, "chars", false);
        const bool includeSpaces = animTextEnabled(params, "includeSpaces", false);
        for (int childIndex = 0; childIndex < parent->GetNumChildren(); ++childIndex){
            Rml::Element* child = parent->GetChild(childIndex);
            auto* textNode = rmlui_dynamic_cast<Rml::ElementText*>(child);
            if (!textNode){
                if (child->GetTagName() != "script" && child->GetTagName() != "style")
                    animTextSplitNode(child, params, needWords, words, chars);
                continue;
            }
            const std::string text = textNode->GetText();
            if (text.empty() || animTextWhitespace(text)) continue;
            const auto segments = animTextSegments(text);
            for (size_t i = 0; i < segments.size(); ++i){
                const auto& segment = segments[i];
                if (segment.space){
                    parent->InsertBefore(animTextNode(parent, segment.text), child);
                    ++childIndex;
                    continue;
                }
                const bool followingSpace = includeSpaces && i + 1 < segments.size() && segments[i + 1].space;
                AnimTextWrapper word;
                if (needWords) word = animTextWrap(parent, "word", params.get("words"), (int)words.size(), (int)words.size());
                Rml::Element* charParent = needWords ? word.unit : parent;
                if (needChars){
                    Rml::Vector<Rml::TextJustification::Cluster> clusters;
                    Rml::TextJustification::GetClusters(segment.text, clusters);
                    for (size_t c = 0; c < clusters.size(); ++c){
                        const size_t begin = (size_t)clusters[c].offset;
                        const size_t end = c + 1 < clusters.size() ? (size_t)clusters[c + 1].offset : segment.text.size();
                        std::string value = segment.text.substr(begin, end - begin);
                        if (followingSpace && c + 1 == clusters.size()) value += "\xc2\xa0";
                        auto character = animTextWrap(charParent, "char", params.get("chars"), (int)chars.size(),
                                                      (int)words.size(), (int)chars.size());
                        character.unit->AppendChild(animTextNode(character.unit, value));
                        chars.push_back(UIElementAccess::wrap(character.unit));
                        if (needWords) charParent->AppendChild(std::move(character.outer));
                        else {
                            parent->InsertBefore(std::move(character.outer), child);
                            ++childIndex;
                        }
                    }
                } else if (needWords){
                    word.unit->AppendChild(animTextNode(word.unit, segment.text + (followingSpace ? "\xc2\xa0" : "")));
                } else {
                    parent->InsertBefore(animTextNode(parent, segment.text), child);
                    ++childIndex;
                }
                if (needWords){
                    words.push_back(UIElementAccess::wrap(word.unit));
                    parent->InsertBefore(std::move(word.outer), child);
                    ++childIndex;
                }
                if (followingSpace) ++i;
            }
            parent->RemoveChild(child);
            --childIndex;
        }
    }

    void animTextSetLine(Rml::Element* element, int line){
        if (element->HasAttribute("data-line")) element->SetAttribute("data-line", std::to_string(line));
        for (int i = 0; i < element->GetNumChildren(); ++i)
            animTextSetLine(element->GetChild(i), line);
    }

    int animTextMarkLines(const std::vector<UIElement>& words, const std::vector<UIElement>& chars){
        const auto& units = !words.empty() ? words : chars;
        int lineIndex = 0;
        float previousTop = 0.f;
        bool first = true;
        for (const UIElement& unit : units){
            auto* element = UIElementAccess::get(unit);
            if (!element) continue;
            const float top = element->GetAbsoluteOffset(Rml::BoxArea::Border).y;
            const float height = element->GetBox().GetSize(Rml::BoxArea::Border).y;
            if (!first && top - previousTop > height * .5f) ++lineIndex;
            animTextSetLine(element, lineIndex);
            previousTop = top;
            first = false;
        }
        return lineIndex;
    }

    void animTextFilterLine(Rml::Element* parent, int line){
        for (int i = parent->GetNumChildren() - 1; i >= 0; --i){
            Rml::Element* child = parent->GetChild(i);
            if (child->GetTagName() == "br" ||
                (child->HasAttribute("data-line") && child->GetAttribute<int>("data-line", -1) != line)){
                // A removed word's adjacent whitespace must not move into another line.
                for (int adjacent : {i + 1, i - 1}){
                    auto* text = adjacent >= 0 && adjacent < parent->GetNumChildren() ?
                        rmlui_dynamic_cast<Rml::ElementText*>(parent->GetChild(adjacent)) : nullptr;
                    if (text && animTextWhitespace(text->GetText())){
                        parent->RemoveChild(text);
                        if (adjacent < i) --i;
                    }
                }
                parent->RemoveChild(child);
            } else if (!rmlui_dynamic_cast<Rml::ElementText*>(child)){
                const bool hadChildren = child->GetNumChildren() > 0;
                animTextFilterLine(child, line);
                if (hadChildren && child->GetNumChildren() == 0) parent->RemoveChild(child);
            }
        }
    }

    void animTextLayout(Rml::Element* element, std::vector<float>& out){
        const auto size = element->GetBox().GetSize(Rml::BoxArea::Content);
        out.push_back(size.x); out.push_back(size.y);
        out.push_back(element->GetComputedValues().font_size());
        out.push_back(element->GetComputedValues().line_height().value);
        for (int i = 0; i < element->GetNumChildren(); ++i) animTextLayout(element->GetChild(i), out);
    }

    bool animTextSameLayout(Rml::Element* element, const std::vector<float>& layout, size_t& index){
        if (index + 4 > layout.size()) return false;
        const auto size = element->GetBox().GetSize(Rml::BoxArea::Content);
        if (layout[index++] != size.x || layout[index++] != size.y ||
            layout[index++] != element->GetComputedValues().font_size() ||
            layout[index++] != element->GetComputedValues().line_height().value) return false;
        for (int i = 0; i < element->GetNumChildren(); ++i){
            if (!animTextSameLayout(element->GetChild(i), layout, index)) return false;
        }
        return true;
    }

    void animTextCollect(Rml::Element* element, const char* attribute, std::vector<UIElement>& out){
        for (int i = 0; i < element->GetNumChildren(); ++i){
            Rml::Element* child = element->GetChild(i);
            if (child->HasAttribute(attribute)) out.push_back(UIElementAccess::wrap(child));
            else animTextCollect(child, attribute, out);
        }
    }

}

AnimText::AnimText(const UIElement& element, const AnimParam& options): target(element), params(options){
    if (auto* root = UIElementAccess::get(target)){
        original = root->GetInnerRML();
        animTextReadingText(root, originalText);
        while (!originalText.empty() && originalText.back() == '\n') originalText.pop_back();
    }
}

AnimText::~AnimText() = default;

std::shared_ptr<AnimText> AnimText::create(const UIElement& target, const AnimParam& params){
    static const uint64_t listener = AnimEngine::instance().addResetListener([](){
        if (!animTextRegistered) return;
        auto splitters = std::move(animTextSplitters());
        animTextSplitters().clear();
        for (const auto& splitter : splitters) splitter->revert();
    });
    (void)listener;
    auto splitter = std::shared_ptr<AnimText>(new AnimText(target, params));
    if (!target.isValid()){
        Log::warn("[HTML UI] anime.splitText: no matching text element");
        splitter->active = false;
        return splitter;
    }
    animTextSplitters().push_back(splitter);
    splitter->rebuild();
    return splitter;
}

void AnimText::rememberLayout(){
    layout.clear();
    if (auto* root = UIElementAccess::get(target)) animTextLayout(root, layout);
}

bool AnimText::layoutChanged() const{
    if (auto* root = UIElementAccess::get(target)){
        size_t index = 0;
        return !animTextSameLayout(root, layout, index) || index != layout.size();
    }
    return false;
}

void AnimText::rebuild(){
    auto* root = UIElementAccess::get(target);
    if (!root || rebuilding) return;
    rebuilding = true;
    auto& registered = animTextSplitters();
    if (std::none_of(registered.begin(), registered.end(), [this](const auto& splitter){ return splitter.get() == this; }))
        registered.push_back(shared_from_this());
    active = true;
    chars.clear(); words.clear(); lines.clear();
    if (ready) root->SetInnerRML(original);
    const bool needLines = animTextEnabled(params, "lines", false);
    const bool needChars = animTextEnabled(params, "chars", false);
    const bool showWords = animTextEnabled(params, "words", true);
    const bool linesOnly = needLines && !showWords && !needChars;
    const bool needWords = showWords || linesOnly;
    if (!needLines && !needChars && !showWords){
        rememberLayout();
        ready = true;
        rebuilding = false;
        return;
    }
    animTextSplitNode(root, params, needWords, words, chars);
    if (Rml::Context* context = root->GetContext()) context->Update();
    const auto& units = !words.empty() ? words : chars;
    const int lineIndex = animTextMarkLines(words, chars);
    if (needLines && !units.empty()){
        std::vector<Rml::ElementPtr> lineNodes;
        for (int i = 0; i <= lineIndex; ++i){
            auto clone = root->Clone();
            animTextFilterLine(clone.get(), i);
            auto line = animTextWrap(root, "line", params.get("lines"), i);
            line.unit->SetAttribute("data-line", std::to_string(i));
            while (clone->GetNumChildren()) line.unit->AppendChild(clone->RemoveChild(clone->GetChild(0)));
            lines.push_back(UIElementAccess::wrap(line.unit));
            lineNodes.push_back(std::move(line.outer));
        }
        root->SetInnerRML("");
        for (auto& line : lineNodes) root->AppendChild(std::move(line));
        words.clear(); chars.clear();
        if (needWords) animTextCollect(root, "data-word", words);
        if (needChars) animTextCollect(root, "data-char", chars);
    }
    if (linesOnly){
        for (const UIElement& word : words){
            auto* unit = UIElementAccess::get(word);
            Rml::Element* parent = unit->GetParentNode();
            while (unit->GetNumChildren()) parent->InsertBefore(unit->RemoveChild(unit->GetChild(0)), unit);
            parent->RemoveChild(unit);
        }
        words.clear();
    }
    if (animTextEnabled(params, "accessible", true)){
        auto accessible = Rml::Factory::InstanceElement(root, "span", "span", {});
        accessible->SetAttribute("data-anime-accessible", "");
        accessible->SetProperty("position", "absolute");
        accessible->SetProperty("width", "1px");
        accessible->SetProperty("height", "1px");
        accessible->SetProperty("overflow-x", "hidden");
        accessible->SetProperty("overflow-y", "hidden");
        accessible->SetProperty("opacity", "0");
        accessible->SetProperty("pointer-events", "none");
        accessible->SetProperty("white-space", "nowrap");
        // Cloning links/controls would duplicate ids and keyboard focus stops. The accessible
        // copy only supplies the original reading order; the visible tree keeps its elements.
        accessible->AppendChild(animTextNode(accessible.get(), originalText));
        root->InsertBefore(std::move(accessible), root->GetChild(0));
        for (const auto* list : {&lines, &words, &chars}){
            for (const auto& element : *list) UIElementAccess::get(element)->SetAttribute("aria-hidden", "true");
        }
    }
    if (Rml::Context* context = root->GetContext()) context->Update();
    rememberLayout();
    ready = true;
    rebuilding = false;
}

AnimText& AnimText::split(){
    if (ready && active && !animTextEnabled(params, "lines", false)){
        if (auto* root = UIElementAccess::get(target)){
            if (Rml::Context* context = root->GetContext()) context->Update();
        }
        animTextMarkLines(words, chars);
        rememberLayout();
    } else {
        rebuild();
    }
    return *this;
}

AnimText& AnimText::refresh(){
    rebuild();
    return *this;
}

AnimText& AnimText::revert(){
    if (auto* root = UIElementAccess::get(target)) root->SetInnerRML(original);
    chars.clear(); words.clear(); lines.clear();
    ready = false;
    active = false;
    return *this;
}

bool AnimText::updateLayouts(Rml::Context* context){
    if (!animTextRegistered) return false;
    bool changed = false;
    auto& splitters = animTextSplitters();
    for (size_t i = 0; i < splitters.size();){
        auto splitter = splitters[i];
        auto* root = UIElementAccess::get(splitter->target);
        if (!root || !splitter->active){
            splitters.erase(splitters.begin() + (std::ptrdiff_t)i);
            continue;
        }
        if (root->GetContext() == context && !splitter->rebuilding && splitter->layoutChanged()){
            if (animTextEnabled(splitter->params, "lines", false)){
                splitter->rebuild();
            } else {
                animTextMarkLines(splitter->words, splitter->chars);
                splitter->rememberLayout();
            }
            changed = true;
        }
        ++i;
    }
    return changed;
}
