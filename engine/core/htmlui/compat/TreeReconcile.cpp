// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/TreeReconcile.h"

#include "htmlui/compat/html/HtmlTransform.h"

#include <algorithm>
#include <cstdlib>
#include <set>

using namespace jasperengine::htmlui::compat;

namespace {

    constexpr std::string_view kLoc = "data-htmlui-loc";
    constexpr std::string_view kTemplateIndex = "data-htmlui-template";

    bool isSpace(char c){
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
    }

    bool isNameChar(char c){
        return !isSpace(c) && c != '>' && c != '/' && c != '=' && c != '"' && c != '\'' && c != '<';
    }

    void appendUtf8(std::string& out, unsigned long code){
        if (code < 0x80){
            out += (char)code;
        }else if (code < 0x800){
            out += (char)(0xC0 | (code >> 6));
            out += (char)(0x80 | (code & 0x3F));
        }else if (code < 0x10000){
            out += (char)(0xE0 | (code >> 12));
            out += (char)(0x80 | ((code >> 6) & 0x3F));
            out += (char)(0x80 | (code & 0x3F));
        }else{
            out += (char)(0xF0 | (code >> 18));
            out += (char)(0x80 | ((code >> 12) & 0x3F));
            out += (char)(0x80 | ((code >> 6) & 0x3F));
            out += (char)(0x80 | (code & 0x3F));
        }
    }

    // the references RmlUi decodes (StringUtilities::DecodeRml): the XML ones and numeric ones
    std::string decode(std::string_view text){
        if (text.find('&') == std::string_view::npos)
            return std::string(text);
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); i++){
            if (text[i] != '&'){
                out += text[i];
                continue;
            }
            const size_t end = text.find(';', i);
            if (end == std::string_view::npos || end - i > 12){
                out += '&';
                continue;
            }
            const std::string_view name = text.substr(i + 1, end - i - 1);
            if (name == "amp") out += '&';
            else if (name == "lt") out += '<';
            else if (name == "gt") out += '>';
            else if (name == "quot") out += '"';
            else if (name == "apos") out += '\'';
            else if (!name.empty() && name[0] == '#'){
                const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
                const std::string digits(name.substr(hex ? 2 : 1));
                char* stop = nullptr;
                const unsigned long code = std::strtoul(digits.c_str(), &stop, hex ? 16 : 10);
                if (digits.empty() || *stop != '\0'){
                    out += '&';
                    continue;
                }
                appendUtf8(out, code);
            }else{
                out += '&';
                continue;
            }
            i = end;
        }
        return out;
    }

    class Reader {
    public:
        explicit Reader(std::string_view text): s(text) {}

        bool fail(const std::string& message){
            if (error.empty())
                error = message + " at byte " + std::to_string(at);
            return false;
        }

        // an element starting at `at` (on its '<')
        bool element(RmlNode& out, int depth = 0){
            if (depth > 1024)
                return fail("elements nested too deep");
            const size_t start = at;
            at++;   // '<'
            const size_t nameStart = at;
            while (at < s.size() && isNameChar(s[at]))
                at++;
            out.kind = RmlNode::Kind::Element;
            out.tag.assign(s.substr(nameStart, at - nameStart));
            if (out.tag.empty())
                return fail("an element without a name");
            // attributes
            while (true){
                while (at < s.size() && isSpace(s[at]))
                    at++;
                if (at >= s.size())
                    return fail("an unclosed start tag");
                if (s[at] == '>'){
                    at++;
                    break;
                }
                if (s[at] == '/' && at + 1 < s.size() && s[at + 1] == '>'){
                    at += 2;
                    out.rml.assign(s.substr(start, at - start));
                    finish(out);
                    return true;
                }
                const size_t attributeStart = at;
                while (at < s.size() && isNameChar(s[at]))
                    at++;
                std::string name(s.substr(attributeStart, at - attributeStart));
                if (name.empty())
                    return fail("an unexpected character in a start tag");
                while (at < s.size() && isSpace(s[at]))
                    at++;
                std::string value;
                if (at < s.size() && s[at] == '='){
                    at++;
                    while (at < s.size() && isSpace(s[at]))
                        at++;
                    if (at >= s.size() || (s[at] != '"' && s[at] != '\''))
                        return fail("an unquoted attribute value");
                    const char quote = s[at++];
                    const size_t end = s.find(quote, at);
                    if (end == std::string_view::npos)
                        return fail("an unclosed attribute value");
                    value = decode(s.substr(at, end - at));
                    at = end + 1;
                }
                out.attributes.emplace_back(std::move(name), std::move(value));
            }
            // an inline SVG is text for RmlUi: kept as one text child
            if (out.tag == "svg")
                return rawContent(out, start);
            // children
            while (true){
                if (at >= s.size())
                    return fail("<" + out.tag + "> is not closed");
                if (s.compare(at, 2, "</") == 0){
                    const size_t close = s.find('>', at);
                    if (close == std::string_view::npos)
                        return fail("an unclosed end tag");
                    at = close + 1;
                    out.rml.assign(s.substr(start, at - start));
                    finish(out);
                    return true;
                }
                if (s.compare(at, 4, "<!--") == 0){
                    const size_t close = s.find("-->", at + 4);
                    if (close == std::string_view::npos)
                        return fail("an unclosed comment");
                    at = close + 3;
                    continue;
                }
                if (s[at] == '<'){
                    RmlNode child;
                    if (!element(child, depth + 1))
                        return false;
                    out.children.push_back(std::move(child));
                    continue;
                }
                const size_t end = s.find('<', at);
                const size_t stop = end == std::string_view::npos ? s.size() : end;
                RmlNode text;
                text.kind = RmlNode::Kind::Text;
                text.text = decode(s.substr(at, stop - at));
                text.rml.assign(s.substr(at, stop - at));
                out.children.push_back(std::move(text));
                at = stop;
            }
        }

        // the content up to the matching end tag, as text (nested elements of the same name counted)
        bool rawContent(RmlNode& out, size_t start){
            const std::string open = "<" + out.tag, close = "</" + out.tag;
            int depth = 1;
            const size_t contentStart = at;
            size_t scan = at;
            while (depth > 0){
                const size_t next = s.find('<', scan);
                if (next == std::string_view::npos)
                    return fail("<" + out.tag + "> is not closed");
                if (s.compare(next, close.size(), close) == 0 && next + close.size() < s.size() && !isNameChar(s[next + close.size()])){
                    if (--depth == 0){
                        if (next > contentStart){
                            RmlNode text;
                            text.kind = RmlNode::Kind::Text;
                            text.text.assign(s.substr(contentStart, next - contentStart));
                            text.rml = text.text;
                            out.children.push_back(std::move(text));
                        }
                        const size_t end = s.find('>', next);
                        if (end == std::string_view::npos)
                            return fail("an unclosed end tag");
                        at = end + 1;
                        out.rml.assign(s.substr(start, at - start));
                        finish(out);
                        return true;
                    }
                }else if (s.compare(next, open.size(), open) == 0 && next + open.size() < s.size() && !isNameChar(s[next + open.size()])){
                    const size_t end = s.find('>', next);
                    if (end != std::string_view::npos && s[end - 1] != '/')
                        depth++;
                }
                scan = next + 1;
            }
            return true;
        }

        static void finish(RmlNode& node){
            if (const std::string* loc = node.attribute(kLoc))
                node.loc = std::atoi(loc->c_str());
        }

        std::string_view s;
        size_t at = 0;
        std::string error;
    };

    // ---- comparisons ----

    bool ignoredForSignature(const std::string& name){
        return name == kLoc || name == kTemplateIndex;
    }

    // the same markup, data-htmlui-loc and template indices aside (a renumbering is no change)
    bool sameMarkup(const RmlNode& a, const RmlNode& b){
        if (a.kind != b.kind)
            return false;
        if (a.kind == RmlNode::Kind::Text)
            return a.text == b.text;
        if (a.tag != b.tag || a.children.size() != b.children.size())
            return false;
        size_t i = 0, j = 0;
        while (true){
            while (i < a.attributes.size() && ignoredForSignature(a.attributes[i].first))
                i++;
            while (j < b.attributes.size() && ignoredForSignature(b.attributes[j].first))
                j++;
            if (i == a.attributes.size() || j == b.attributes.size()){
                if (i != a.attributes.size() || j != b.attributes.size())
                    return false;
                break;
            }
            if (a.attributes[i] != b.attributes[j])
                return false;
            i++;
            j++;
        }
        for (size_t k = 0; k < a.children.size(); k++){
            if (!sameMarkup(a.children[k], b.children[k]))
                return false;
        }
        return true;
    }

    bool sameMarkup(const std::vector<RmlNode>& a, const std::vector<RmlNode>& b){
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++){
            if (!sameMarkup(a[i], b[i]))
                return false;
        }
        return true;
    }

    std::vector<std::string> classesOf(const RmlNode& node){
        std::vector<std::string> classes;
        if (const std::string* value = node.attribute("class")){
            size_t at = 0;
            while (at < value->size()){
                while (at < value->size() && isSpace((*value)[at]))
                    at++;
                const size_t start = at;
                while (at < value->size() && !isSpace((*value)[at]))
                    at++;
                if (at > start)
                    classes.emplace_back(value->substr(start, at - start));
            }
        }
        return classes;
    }

    bool holdsAll(const std::vector<std::string>& set, const std::vector<std::string>& subset){
        for (const std::string& name : subset){
            if (std::find(set.begin(), set.end(), name) == set.end())
                return false;
        }
        return true;
    }

    bool isBinding(const std::string& name){
        return name == "data-model" || isBindingAttribute(name);
    }

    bool hasDataFor(const RmlNode& node){
        return node.attribute("data-for") != nullptr;
    }

    bool isView(const std::string& tag){
        return tag == "list-view" || tag == "grid-view" || tag == "tree-view" || tag == "table-view";
    }

    std::string describe(const RmlNode& node){
        std::string text = "<" + node.tag;
        if (const std::string* id = node.attribute("id"))
            text += "#" + *id;
        return text + ">";
    }

    // ---- the plan ----

    class Planner {
    public:
        explicit Planner(ReconcilePlan& plan): plan(plan) {}

        // for each new child, the matched old child's index (-1: inserted)
        std::vector<int> matchChildren(const std::vector<RmlNode>& olds, const std::vector<RmlNode>& news){
            std::vector<int> match(news.size(), -1);
            std::vector<bool> used(olds.size(), false);
            // ids first
            for (size_t n = 0; n < news.size(); n++){
                const RmlNode& node = news[n];
                const std::string* id = node.isElement() ? node.attribute("id") : nullptr;
                if (!id || id->empty())
                    continue;
                for (size_t o = 0; o < olds.size(); o++){
                    const std::string* oldId = olds[o].isElement() ? olds[o].attribute("id") : nullptr;
                    if (!used[o] && oldId && *oldId == *id){
                        match[n] = (int)o;
                        used[o] = true;
                        break;
                    }
                }
            }
            // then the tag (same classes, classes that hold the new ones, any), after the previous match first
            int last = -1;
            for (size_t n = 0; n < news.size(); n++){
                const RmlNode& node = news[n];
                if (!node.isElement()){
                    continue;
                }
                if (match[n] >= 0){
                    last = match[n];
                    continue;
                }
                const std::string* id = node.attribute("id");
                const std::vector<std::string> classes = classesOf(node);
                int found = -1;
                for (int pass = 0; pass < 3 && found < 0; pass++){
                    for (int round = 0; round < 2 && found < 0; round++){
                        const size_t begin = round == 0 ? (size_t)(last + 1) : 0;
                        const size_t end = round == 0 ? olds.size() : (size_t)std::max(0, last + 1);
                        for (size_t o = begin; o < end; o++){
                            const RmlNode& old = olds[o];
                            if (used[o] || !old.isElement() || old.tag != node.tag)
                                continue;
                            const std::string* oldId = old.attribute("id");
                            if (oldId && !oldId->empty() && (!id || *id != *oldId))
                                continue;   // another element's id
                            const std::vector<std::string> oldClasses = classesOf(old);
                            const bool fits = pass == 2 || (pass == 0 ? (oldClasses.size() == classes.size() && holdsAll(oldClasses, classes))
                                                                      : holdsAll(oldClasses, classes));
                            if (fits){
                                found = (int)o;
                                break;
                            }
                        }
                    }
                }
                if (found >= 0){
                    match[n] = found;
                    used[(size_t)found] = true;
                    last = found;
                }
            }
            // texts by position among the texts
            std::vector<int> oldTexts;
            for (size_t o = 0; o < olds.size(); o++){
                if (!olds[o].isElement())
                    oldTexts.push_back((int)o);
            }
            size_t textIndex = 0;
            for (size_t n = 0; n < news.size(); n++){
                if (news[n].isElement())
                    continue;
                if (textIndex < oldTexts.size())
                    match[n] = oldTexts[textIndex];
                textIndex++;
            }
            return match;
        }

        // the locs of two subtrees with the same markup, in parallel
        void mapLocs(const RmlNode& a, const RmlNode& b){
            if (a.loc >= 0 && b.loc >= 0)
                plan.locs[a.loc] = b.loc;
            for (size_t i = 0; i < a.children.size() && i < b.children.size(); i++)
                mapLocs(a.children[i], b.children[i]);
        }

        // the locs of matched subtrees whose markup may differ (template contents)
        void mapMatched(const std::vector<RmlNode>& olds, const std::vector<RmlNode>& news){
            const std::vector<int> match = matchChildren(olds, news);
            for (size_t n = 0; n < news.size(); n++){
                if (match[n] < 0 || !news[n].isElement())
                    continue;
                const RmlNode& old = olds[(size_t)match[n]];
                if (old.loc >= 0 && news[n].loc >= 0)
                    plan.locs[old.loc] = news[n].loc;
                if (old.tag == news[n].tag)
                    mapMatched(old.children, news[n].children);
            }
        }

        void replace(ReconcilePlan::Element& out, const RmlNode& node, const std::string& reason){
            out.replace = true;
            out.reason = reason;
            out.children.clear();
            out.removedChildren.clear();
            plan.replaced++;
            plan.causes.push_back(describe(node) + ": " + reason);
        }

        void attributes(ReconcilePlan::Element& out, const RmlNode& old, const RmlNode& node){
            std::set<std::string> names;
            for (const auto& pair : old.attributes)
                names.insert(pair.first);
            for (const auto& pair : node.attributes)
                names.insert(pair.first);
            for (const std::string& name : names){
                if (name == kLoc)
                    continue;   // renumbered for every element (the plan's locs)
                const std::string* before = old.attribute(name);
                const std::string* after = node.attribute(name);
                if (name == "class"){
                    const std::vector<std::string> oldClasses = classesOf(old), newClasses = classesOf(node);
                    for (const std::string& each : newClasses){
                        if (std::find(oldClasses.begin(), oldClasses.end(), each) == oldClasses.end())
                            out.addClasses.push_back(each);
                    }
                    for (const std::string& each : oldClasses){
                        if (std::find(newClasses.begin(), newClasses.end(), each) == newClasses.end())
                            out.removeClasses.push_back(each);
                    }
                    continue;
                }
                if (name == "style"){
                    const auto oldStyle = parseDeclarations(before ? *before : std::string());
                    const auto newStyle = parseDeclarations(after ? *after : std::string());
                    for (const auto& declaration : newStyle){
                        auto it = std::find_if(oldStyle.begin(), oldStyle.end(), [&](const auto& each){ return each.first == declaration.first; });
                        if (it == oldStyle.end() || it->second != declaration.second)
                            out.setStyles.push_back(declaration);
                    }
                    for (const auto& declaration : oldStyle){
                        auto it = std::find_if(newStyle.begin(), newStyle.end(), [&](const auto& each){ return each.first == declaration.first; });
                        if (it == newStyle.end())
                            out.setStyles.emplace_back(declaration.first, std::string());
                    }
                    continue;
                }
                if (before && after && *before == *after)
                    continue;
                if (!before && !after)
                    continue;
                if (after)
                    out.setAttributes.emplace_back(name, *after);
                else
                    out.removeAttributes.push_back(name);
            }
        }

        bool bindingChanged(const RmlNode& old, const RmlNode& node, std::string& cause){
            std::set<std::string> names;
            for (const auto& pair : old.attributes){
                if (isBinding(pair.first))
                    names.insert(pair.first);
            }
            for (const auto& pair : node.attributes){
                if (isBinding(pair.first))
                    names.insert(pair.first);
            }
            for (const std::string& name : names){
                const std::string* before = old.attribute(name);
                const std::string* after = node.attribute(name);
                if (!before || !after || *before != *after){
                    cause = name + " changed";
                    return true;
                }
            }
            return false;
        }

        // a pair of elements with the same tag
        void element(const RmlNode& old, const RmlNode& node, ReconcilePlan::Element& out){
            out.oldLoc = old.loc;
            out.newLoc = node.loc;
            out.tag = node.tag;
            out.node = &node;
            if (old.loc >= 0 && node.loc >= 0)
                plan.locs[old.loc] = node.loc;
            if (old.tag != node.tag){
                replace(out, node, "its element type changed");
                return;
            }
            std::string cause;
            if (bindingChanged(old, node, cause)){
                replace(out, node, cause);
                return;
            }
            // what RmlUi keeps as markup, not elements: changed means replaced
            if (node.tag == "svg" || hasDataFor(node)){
                if (!sameMarkup(old.children, node.children)){
                    replace(out, node, node.tag == "svg" ? "its SVG changed" : "its data-for template changed");
                    return;
                }
                mapLocs(old, node);
                attributes(out, old, node);
                out.leaf = true;
                count(out);
                return;
            }
            attributes(out, old, node);
            // a view's rows are its own; its template placeholders and columns are compared as markup
            if (isView(node.tag)){
                if (!sameMarkup(old.children, node.children)){
                    replace(out, node, "its columns or templates changed");
                    return;
                }
                mapLocs(old, node);
                out.leaf = true;
                count(out);
                return;
            }
            // the engine renders a <markdown>'s content: replaced when the author changed it
            if (node.tag == "markdown"){
                out.leaf = true;
                if (!sameMarkup(old.children, node.children)){
                    out.content = true;
                    out.contentRml = node.innerRml();
                }
                count(out);
                return;
            }
            // a {{ }} text that changed is a binding change of the element
            const std::vector<int> match = matchChildren(old.children, node.children);
            for (size_t n = 0; n < node.children.size(); n++){
                const RmlNode& child = node.children[n];
                if (child.isElement())
                    continue;
                const RmlNode* before = match[n] >= 0 ? &old.children[(size_t)match[n]] : nullptr;
                const bool bound = child.text.find("{{") != std::string::npos || (before && before->text.find("{{") != std::string::npos);
                if (bound && (!before || before->text != child.text)){
                    replace(out, node, "a {{ }} text changed");
                    return;
                }
            }
            for (const RmlNode& child : old.children){
                if (!child.isElement() && child.text.find("{{") != std::string::npos){
                    bool kept = false;
                    for (int index : match)
                        kept |= index >= 0 && &old.children[(size_t)index] == &child;
                    if (!kept){
                        replace(out, node, "a {{ }} text changed");
                        return;
                    }
                }
            }

            std::vector<bool> matched(old.children.size(), false);
            int textIndex = 0;
            std::vector<int> textIndexOf(old.children.size(), -1);
            for (size_t o = 0; o < old.children.size(); o++){
                if (!old.children[o].isElement())
                    textIndexOf[o] = textIndex++;
            }
            for (size_t n = 0; n < node.children.size(); n++){
                const RmlNode& child = node.children[n];
                ReconcilePlan::Child entry;
                entry.node = &child;
                if (match[n] < 0){
                    entry.kind = ReconcilePlan::Child::Kind::Insert;
                    if (child.isElement())
                        plan.inserted++;
                    out.children.push_back(std::move(entry));
                    continue;
                }
                const size_t o = (size_t)match[n];
                matched[o] = true;
                entry.oldIndex = (int)o;
                if (!child.isElement()){
                    entry.kind = ReconcilePlan::Child::Kind::Text;
                    entry.textIndex = textIndexOf[o];
                    entry.textChanged = old.children[o].text != child.text;
                    if (entry.textChanged)
                        entry.text = child.text;
                    out.children.push_back(std::move(entry));
                    continue;
                }
                entry.kind = ReconcilePlan::Child::Kind::Element;
                entry.element.emplace_back();
                element(old.children[o], child, entry.element.back());
                out.children.push_back(std::move(entry));
            }
            for (size_t o = 0; o < old.children.size(); o++){
                if (matched[o])
                    continue;
                ReconcilePlan::Removed removed;
                removed.text = !old.children[o].isElement();
                removed.loc = old.children[o].loc;
                removed.textIndex = textIndexOf[o];
                out.removedChildren.push_back(removed);
                if (!removed.text)
                    plan.removed++;
            }
            count(out);
        }

        void count(const ReconcilePlan::Element& out){
            bool own = !out.setAttributes.empty() || !out.removeAttributes.empty() || !out.addClasses.empty() ||
                       !out.removeClasses.empty() || !out.setStyles.empty() || out.content;
            for (const ReconcilePlan::Child& child : out.children)
                own |= child.kind == ReconcilePlan::Child::Kind::Text && child.textChanged;
            if (own)
                plan.updated++;
            else
                plan.kept++;
        }

        // templates: matched through their placeholders' indices
        void templates(const RmlNode& oldRoot, const RmlNode& newRoot, const std::vector<std::vector<RmlNode>>& oldTemplates,
                       const std::vector<std::vector<RmlNode>>& newTemplates){
            // the template placeholders of both trees by loc (one walk each: a lookup per matched
            // loc would walk the tree once per element)
            std::unordered_map<int, const RmlNode*> oldPlaceholders, newPlaceholders;
            placeholders(oldRoot, oldPlaceholders);
            placeholders(newRoot, newPlaceholders);
            std::vector<std::pair<int, int>> pairs;
            for (const auto& [oldLoc, before] : oldPlaceholders){
                auto mapped = plan.locs.find(oldLoc);
                if (mapped != plan.locs.end())
                    pairs.emplace_back(oldLoc, mapped->second);
            }
            std::sort(pairs.begin(), pairs.end());
            for (const auto& [oldLoc, newLoc] : pairs){
                auto after = newPlaceholders.find(newLoc);
                if (after == newPlaceholders.end())
                    continue;
                const std::string* oldIndex = oldPlaceholders[oldLoc]->attribute(kTemplateIndex);
                const std::string* newIndex = after->second->attribute(kTemplateIndex);
                if (!oldIndex || !newIndex)
                    continue;
                const int from = std::atoi(oldIndex->c_str()), to = std::atoi(newIndex->c_str());
                if (from < 0 || to < 0 || (size_t)from >= oldTemplates.size() || (size_t)to >= newTemplates.size())
                    continue;
                plan.templates[from] = to;
                if (!sameMarkup(oldTemplates[(size_t)from], newTemplates[(size_t)to]))
                    plan.changedTemplates.insert(to);
                mapMatched(oldTemplates[(size_t)from], newTemplates[(size_t)to]);
            }
        }

        static void placeholders(const RmlNode& node, std::unordered_map<int, const RmlNode*>& out){
            if (node.loc >= 0 && node.attribute(kTemplateIndex))
                out[node.loc] = &node;
            for (const RmlNode& child : node.children){
                if (child.isElement())
                    placeholders(child, out);
            }
        }

    private:
        ReconcilePlan& plan;
    };

}

const std::string* RmlNode::attribute(std::string_view name) const{
    for (const auto& pair : attributes){
        if (pair.first == name)
            return &pair.second;
    }
    return nullptr;
}

std::string RmlNode::innerRml() const{
    std::string out;
    for (const RmlNode& child : children)
        out += child.rml;
    return out;
}

bool jasperengine::htmlui::compat::parseRmlElement(std::string_view rml, std::string_view tag, RmlNode& out, std::string* error){
    const std::string open = "<" + std::string(tag);
    size_t at = 0;
    while (true){
        at = rml.find(open, at);
        if (at == std::string_view::npos){
            if (error)
                *error = "no <" + std::string(tag) + "> element";
            return false;
        }
        if (at + open.size() < rml.size() && !isNameChar(rml[at + open.size()]))
            break;
        at += open.size();
    }
    Reader reader(rml);
    reader.at = at;
    out = RmlNode();
    if (!reader.element(out)){
        if (error)
            *error = reader.error;
        return false;
    }
    return true;
}

bool jasperengine::htmlui::compat::parseRmlFragment(std::string_view rml, std::vector<RmlNode>& out, std::string* error){
    const std::string wrapped = "<fragment>" + std::string(rml) + "</fragment>";
    RmlNode root;
    if (!parseRmlElement(wrapped, "fragment", root, error))
        return false;
    // the slices point into `wrapped`: copies (RmlNode::rml is a string)
    out = std::move(root.children);
    return true;
}

std::vector<std::pair<std::string, std::string>> jasperengine::htmlui::compat::parseDeclarations(std::string_view style){
    std::vector<std::pair<std::string, std::string>> declarations;
    size_t at = 0;
    while (at < style.size()){
        // a ';' inside parentheses or quotes is part of the value (url(), strings)
        size_t end = at;
        int parens = 0;
        char quote = 0;
        for (; end < style.size(); end++){
            const char c = style[end];
            if (quote){
                if (c == quote)
                    quote = 0;
            }else if (c == '"' || c == '\''){
                quote = c;
            }else if (c == '('){
                parens++;
            }else if (c == ')'){
                parens = std::max(0, parens - 1);
            }else if (c == ';' && parens == 0){
                break;
            }
        }
        const std::string_view declaration = style.substr(at, end - at);
        const size_t colon = declaration.find(':');
        if (colon != std::string_view::npos){
            auto trim = [](std::string_view text){
                while (!text.empty() && isSpace(text.front()))
                    text.remove_prefix(1);
                while (!text.empty() && isSpace(text.back()))
                    text.remove_suffix(1);
                return std::string(text);
            };
            std::string name = trim(declaration.substr(0, colon));
            std::string value = trim(declaration.substr(colon + 1));
            if (!name.empty())
                declarations.emplace_back(std::move(name), std::move(value));
        }
        at = end + 1;
    }
    return declarations;
}

bool ReconcilePlan::Element::changed() const{
    return replace || content || !setAttributes.empty() || !removeAttributes.empty() || !addClasses.empty() ||
           !removeClasses.empty() || !setStyles.empty();
}

ReconcilePlan jasperengine::htmlui::compat::reconcileTrees(const RmlNode& oldRoot, const RmlNode& newRoot,
                                                          const std::vector<std::vector<RmlNode>>& oldTemplates,
                                                          const std::vector<std::vector<RmlNode>>& newTemplates){
    ReconcilePlan plan;
    Planner planner(plan);
    planner.element(oldRoot, newRoot, plan.root);
    planner.templates(oldRoot, newRoot, oldTemplates, newTemplates);
    return plan;
}
