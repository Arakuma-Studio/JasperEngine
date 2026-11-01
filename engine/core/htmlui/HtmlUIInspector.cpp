// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIInspector.h"
#include "HtmlUIInspectorInternal.h"
#include "media/UIVideoControls.h"
#include "media/UIVideoElement.h"

#include "Scene.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/LuaHooks.h"
#include "htmlui/compat/RmlLogTranslator.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/runtime/HtmlFormSupport.h"
#include "htmlui/effects/EffectInstancers.h"
#include "htmlui/effects/EffectRegistry.h"
#include "htmlui/effects/EffectRuntime.h"
#include "object/ui/UIElementInternal.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementScroll.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include "../../libs/rmlui/Source/Core/ElementStyle.h"
#include "../../libs/rmlui/Source/Core/TransformState.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::htmlui;
using namespace jasperengine::htmlui::inspect;
namespace hc = jasperengine::htmlui::compat;

namespace {

    constexpr size_t kTextLimit = 120;   // code points of InspectElement::text

    const char* const kPseudoClasses[] = {"hover", "active", "focus", "checked", "disabled"};

    bool isIdentStart(char c){
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    bool isIdentChar(char c){
        return isIdentStart(c) || (c >= '0' && c <= '9') || c == '-';
    }

    // an id `#id` can name without escapes
    bool isPlainIdent(const std::string& name){
        if (name.empty())
            return false;
        size_t i = 0;
        if (name[0] == '-'){
            if (name.size() < 2)
                return false;
            i = 1;
        }
        if (!isIdentStart(name[i]))
            return false;
        for (size_t j = i + 1; j < name.size(); j++){
            if (!isIdentChar(name[j]))
                return false;
        }
        return true;
    }

    std::string trim(std::string_view text){
        size_t begin = 0, end = text.size();
        while (begin < end && (unsigned char)text[begin] <= ' ')
            begin++;
        while (end > begin && (unsigned char)text[end - 1] <= ' ')
            end--;
        return std::string(text.substr(begin, end - begin));
    }

    // white space runs to one space, trimmed
    std::string collapse(const std::string& text){
        std::string out;
        out.reserve(text.size());
        bool space = false;
        for (char c : text){
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'){
                space = true;
                continue;
            }
            if (space && !out.empty())
                out += ' ';
            space = false;
            out += c;
        }
        return out;
    }

    // at most `limit` code points, "..." appended when cut
    std::string truncateUtf8(const std::string& text, size_t limit){
        size_t points = 0;
        for (size_t i = 0; i < text.size(); i++){
            if (((unsigned char)text[i] & 0xC0) == 0x80)
                continue;
            if (points == limit)
                return text.substr(0, i) + "...";
            points++;
        }
        return text;
    }

    bool hasSvgAncestor(const Rml::Element* element){
        for (const Rml::Element* walk = element->GetParentNode(); walk; walk = walk->GetParentNode()){
            if (walk->GetTagName() == "svg")
                return true;
        }
        return false;
    }

    // the selector's error as a sentence: "selector \"div[[\" is not valid: ..."
    std::string invalidSelector(const std::string& selector, const std::string& why){
        std::string reason = why;
        const std::string prefix = "invalid selector '" + selector + "' (";
        if (reason.compare(0, prefix.size(), prefix) == 0 && !reason.empty() && reason.back() == ')')
            reason = reason.substr(prefix.size(), reason.size() - prefix.size() - 1);
        return "selector \"" + selector + "\" is not valid: " + reason;
    }

    // the first pseudo-element a selector names ("::before", ":after"), "" when none: they are
    // not elements (RmlUi keeps them out of the DOM), so a query cannot return them
    std::string pseudoElementOf(std::string_view selector){
        char quote = 0;
        int brackets = 0;
        for (size_t i = 0; i < selector.size(); i++){
            const char c = selector[i];
            if (quote){
                if (c == '\\')
                    i++;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (c == '"' || c == '\''){
                quote = c;
            }else if (c == '['){
                brackets++;
            }else if (c == ']'){
                brackets--;
            }else if (c == ':' && brackets == 0){
                const bool doubled = i + 1 < selector.size() && selector[i + 1] == ':';
                size_t end = i + (doubled ? 2 : 1);
                std::string name;
                while (end < selector.size() && (isIdentChar(selector[end]) || (unsigned char)selector[end] >= 0x80))
                    name += (char)std::tolower((unsigned char)selector[end++]);
                if (doubled || name == "before" || name == "after" || name == "first-line" || name == "first-letter")
                    return (doubled ? "::" : ":") + name;
                i = end - 1;
            }
        }
        return std::string();
    }

    // a matching selector, the warnings RmlUi gives for an empty one collected instead of logged
    bool matches(Rml::Element* element, const std::string& rmlSelector){
        hc::RmlLogTranslator::Capture capture;
        return element->Matches(rmlSelector);
    }

    // the nearest transform state of the element or an ancestor (what UIElement::getBoundingClientRect uses)
    const Rml::TransformState* transformStateOf(Rml::Element* element){
        for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
            if (const Rml::TransformState* state = walk->GetTransformState())
                return state;
        }
        return nullptr;
    }

    InspectRect transformed(Rml::Element* element, const InspectRect& rect){
        const Rml::TransformState* state = transformStateOf(element);
        const Rml::Matrix4f* transform = state ? state->GetTransform() : nullptr;
        if (!transform)
            return rect;
        const Rml::Vector2f corners[4] = {
            {rect.x, rect.y}, {rect.x + rect.width, rect.y},
            {rect.x, rect.y + rect.height}, {rect.x + rect.width, rect.y + rect.height}};
        float left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
        for (const Rml::Vector2f& corner : corners){
            Rml::Vector4f projected = (*transform) * Rml::Vector4f(corner.x, corner.y, 0.0f, 1.0f);
            if (projected.w != 0.0f){
                projected.x /= projected.w;
                projected.y /= projected.w;
            }
            left = std::min(left, projected.x);
            top = std::min(top, projected.y);
            right = std::max(right, projected.x);
            bottom = std::max(bottom, projected.y);
        }
        return InspectRect{left, top, right - left, bottom - top};
    }

    // ---- computed styles ----

    // a property a CSS name asks for
    struct StyleTarget {
        enum class Kind { Longhand, Shorthand, Custom, Unsupported };
        Kind kind = Kind::Longhand;
        std::string css;
        Rml::PropertyId id = Rml::PropertyId::Invalid;
        Rml::ShorthandId shorthand = Rml::ShorthandId::Invalid;
    };

    // CSS names of every RmlUi property and shorthand (compat::cssPropertyName), once
    struct PropertyNames {
        std::map<std::string, std::vector<Rml::PropertyId>> longhands;   // CSS name -> RmlUi properties
        std::vector<std::pair<std::string, Rml::PropertyId>> sorted;     // every longhand, by CSS name
        std::set<std::string> known;                                     // for suggestions

        PropertyNames(){
            for (Rml::PropertyId id : Rml::StyleSheetSpecification::GetRegisteredProperties()){
                const std::string& rml = Rml::StyleSheetSpecification::GetPropertyName(id);
                if (rml.empty())
                    continue;
                const std::string css = hc::cssPropertyName(rml);
                longhands[css].push_back(id);
                sorted.emplace_back(css, id);
                known.insert(css);
            }
            std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
            for (const hc::PropertySupport& row : hc::propertyTable())
                known.insert(row.name);
        }
    };

    const PropertyNames& propertyNames(){
        // RmlUi registers its properties once (Rml::Initialise); a re-initialisation registers the same ones
        static const PropertyNames names;
        return names;
    }

    size_t editDistance(const std::string& a, const std::string& b){
        std::vector<size_t> row(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++)
            row[j] = j;
        for (size_t i = 1; i <= a.size(); i++){
            size_t diagonal = row[0];
            row[0] = i;
            for (size_t j = 1; j <= b.size(); j++){
                const size_t up = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
                diagonal = up;
            }
        }
        return row[b.size()];
    }

    std::string unknownProperty(const std::string& name){
        std::string best;
        size_t bestDistance = std::max<size_t>(2, name.size() / 3) + 1;
        for (const std::string& known : propertyNames().known){
            const size_t distance = editDistance(name, known);
            if (distance < bestDistance){
                bestDistance = distance;
                best = known;
            }
        }
        return "unknown CSS property \"" + name + "\"" + (best.empty() ? std::string() : " (did you mean " + best + "?)");
    }

    bool resolveTarget(const std::string& requested, StyleTarget& target, std::string& error){
        const std::string name = trim(requested);
        target = StyleTarget();
        target.css = name;
        if (name.size() > 2 && name[0] == '-' && name[1] == '-'){
            target.kind = StyleTarget::Kind::Custom;
            return true;
        }
        std::string lower = name;
        for (char& c : lower)
            c = (char)std::tolower((unsigned char)c);
        target.css = lower;
        const PropertyNames& names = propertyNames();
        auto it = names.longhands.find(lower);
        if (it != names.longhands.end()){
            target.kind = StyleTarget::Kind::Longhand;
            // the RmlUi property of the same name first (a CSS name several RmlUi properties map to)
            target.id = it->second.front();
            for (Rml::PropertyId id : it->second){
                if (Rml::StyleSheetSpecification::GetPropertyName(id) == lower)
                    target.id = id;
            }
            return true;
        }
        const Rml::ShorthandId shorthand = Rml::StyleSheetSpecification::GetShorthandId(lower);
        if (shorthand != Rml::ShorthandId::Invalid){
            target.kind = StyleTarget::Kind::Shorthand;
            target.shorthand = shorthand;
            return true;
        }
        if (hc::findProperty(lower)){
            target.kind = StyleTarget::Kind::Unsupported;
            return true;
        }
        error = unknownProperty(name);
        return false;
    }

    std::string colourText(const Rml::Colourb& colour){
        if (colour.alpha == 255)
            return "rgb(" + std::to_string(colour.red) + ", " + std::to_string(colour.green) + ", " + std::to_string(colour.blue) + ")";
        return "rgba(" + std::to_string(colour.red) + ", " + std::to_string(colour.green) + ", " + std::to_string(colour.blue) + ", " +
               number(colour.alpha / 255.0, 2) + ")";
    }

    // the computed value of an RmlUi property as CSS text
    std::string valueText(Rml::Element* element, Rml::PropertyId id){
        // what Chromium's theme adjusts (a themed checkbox's box, a button's display): its value
        std::string themed;
        if (hc::forms::themedComputedValue(element, id, themed))
            return themed;
        // a line-clamp container computes to flow-root or inline-block, as Chromium's getComputedStyle (fork, P6-L1)
        if (id == Rml::PropertyId::Display && element->GetComputedValues().line_clamp() > 0)
            return element->GetComputedValues().display() == Rml::Style::Display::InlineBlock ? "inline-block" : "flow-root";
        const Rml::Property* found = element->GetProperty(id);
        if (!found)
            return std::string();
        Rml::Property property = *found;
        property.important = false;
        const Rml::Unit unit = property.unit;
        switch (unit){
            case Rml::Unit::COLOUR:
                return colourText(property.Get<Rml::Colourb>());
            case Rml::Unit::PX:
                return number(property.Get<float>()) + "px";
            case Rml::Unit::PERCENT:
                return number(property.Get<float>()) + "%";
            case Rml::Unit::NUMBER:
                return number(property.Get<float>(), 3);
            default:
                break;
        }
        if (Rml::Any(unit & Rml::Unit::LENGTH)){
            if (id == Rml::PropertyId::FontSize)
                return number(element->GetComputedValues().font_size()) + "px";
            return number(element->ResolveLength(property.GetNumericValue())) + "px";
        }
        std::string text = property.ToString();
        if (unit == Rml::Unit::KEYWORD && text == "currentcolor")
            return colourText(element->GetComputedValues().color());
        return text;
    }

    // the RCSS sheets of a compiled document as RmlUi names them, for Property::source
    struct SheetMap {
        struct Sheet {
            std::string url;                      // normalized as RmlLogTranslator does
            uint32_t firstLine = 1;
            const hc::StylesheetOutput* output = nullptr;
            bool hints = false;
            std::vector<std::string_view> lines;  // of output->rcss
        };
        std::vector<Sheet> sheets;

        explicit SheetMap(const hc::CompatDocument& compiled){
            for (size_t i = 0; i < compiled.stylesheets.size(); i++){
                const hc::StylesheetOutput& sheet = compiled.stylesheets[i];
                if (sheet.kind == hc::StylesheetInput::Kind::File)
                    add(sheet.virtualPath, 1, &sheet, false);
                else if (i < compiled.stylesheetLines.size() && compiled.stylesheetLines[i] > 0)
                    add(compiled.sourcePath, compiled.stylesheetLines[i], &sheet, false);
            }
            if (compiled.hintsLine > 0)
                add(compiled.sourcePath, compiled.hintsLine, &compiled.hints, true);
            if (compiled.importantLine > 0)
                add(compiled.sourcePath, compiled.importantLine, &compiled.important, false);
        }

        void add(const std::string& url, uint32_t firstLine, const hc::StylesheetOutput* output, bool hints){
            Sheet sheet;
            sheet.url = hc::RmlLogTranslator::normalizeUrl(url);
            sheet.firstLine = firstLine;
            sheet.output = output;
            sheet.hints = hints;
            std::string_view rcss(output->rcss);
            size_t start = 0;
            while (start < rcss.size()){
                size_t end = rcss.find('\n', start);
                if (end == std::string_view::npos)
                    end = rcss.size();
                sheet.lines.push_back(rcss.substr(start, end - start));
                start = end + 1;
            }
            sheets.push_back(std::move(sheet));
        }

        // the narrowest sheet holding that line (as RmlLogTranslator::translate picks)
        const Sheet* find(const std::string& url, int line) const{
            const std::string normalized = hc::RmlLogTranslator::normalizeUrl(url);
            const Sheet* best = nullptr;
            for (const Sheet& sheet : sheets){
                if (sheet.url != normalized || line < (int)sheet.firstLine)
                    continue;
                if ((size_t)(line - (int)sheet.firstLine) >= sheet.lines.size())
                    continue;
                if (!best || sheet.lines.size() < best->lines.size())
                    best = &sheet;
            }
            return best;
        }
    };

    bool endsWithBrace(std::string_view line){
        const std::string text = trim(line);
        return !text.empty() && text.back() == '{';
    }

    // does an RCSS declaration of `name` set property `id` (itself or through a shorthand)?
    bool declares(const std::string& name, Rml::PropertyId id){
        if (name == Rml::StyleSheetSpecification::GetPropertyName(id))
            return true;
        const Rml::ShorthandId shorthand = Rml::StyleSheetSpecification::GetShorthandId(name);
        return shorthand != Rml::ShorthandId::Invalid && Rml::StyleSheetSpecification::GetShorthandUnderlyingProperties(shorthand).Contains(id);
    }

    // the selector list of a rule as written (the rule's text up to its block), split at top-level commas
    std::vector<std::string> splitSelectors(std::string_view prelude){
        std::vector<std::string> out;
        int depth = 0;
        char quote = 0;
        size_t start = 0;
        for (size_t i = 0; i < prelude.size(); i++){
            const char c = prelude[i];
            if (quote){
                if (c == '\\')
                    i++;
                else if (c == quote)
                    quote = 0;
            }else if (c == '"' || c == '\''){
                quote = c;
            }else if (c == '(' || c == '['){
                depth++;
            }else if (c == ')' || c == ']'){
                depth--;
            }else if (c == ',' && depth == 0){
                out.push_back(trim(prelude.substr(start, i - start)));
                start = i + 1;
            }
        }
        out.push_back(trim(prelude.substr(start)));
        return out;
    }

    std::string preludeOf(std::string_view rule){
        char quote = 0;
        for (size_t i = 0; i < rule.size(); i++){
            const char c = rule[i];
            if (quote){
                if (c == '\\')
                    i++;
                else if (c == quote)
                    quote = 0;
            }else if (c == '"' || c == '\''){
                quote = c;
            }else if (c == '{'){
                return trim(rule.substr(0, i));
            }
        }
        return trim(rule);
    }

    void setLocation(InspectStyle& style, const std::string& file, uint32_t line, uint32_t column, const std::string& suffix = std::string()){
        style.file = file;
        style.line = line;
        style.column = column;
        style.source = file;
        if (line > 0){
            style.source += ":" + std::to_string(line);
            if (column > 0)
                style.source += ":" + std::to_string(column);
        }
        style.source += suffix;
    }

    class StyleSources {
    public:
        StyleSources(const Doc& doc, Refs& refs): doc(doc), refs(refs){
            if (doc.compiled)
                sheets.reset(new SheetMap(*doc.compiled));
        }

        void locate(Rml::Element* element, Rml::PropertyId id, InspectStyle& style){
            if (const Rml::Property* local = element->GetLocalProperty(id)){
                locateLocal(element, id, *local, style);
                return;
            }
            const Rml::PropertyDefinition* definition = Rml::StyleSheetSpecification::GetProperty(id);
            if (definition && definition->IsInherited()){
                for (Rml::Element* ancestor = element->GetParentNode(); ancestor; ancestor = ancestor->GetParentNode()){
                    if (ancestor->GetLocalProperty(id)){
                        style.source = "inherited from " + refs.of(ancestor);
                        return;
                    }
                    if (ancestor == doc.document)
                        break;
                }
            }
            style.source = "default";
        }

        void locateCustom(Rml::Element* element, const std::string& name, InspectStyle& style){
            for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
                if (const Rml::Property* local = walk->GetStyle()->GetLocalCustomProperty(name)){
                    if (walk == element)
                        locateSource(element, *local, std::string(), Rml::PropertyId::Invalid, style);
                    else
                        style.source = "inherited from " + refs.of(walk);
                    return;
                }
                if (walk == doc.document)
                    break;
            }
            style.source = "default";
        }

    private:
        void locateLocal(Rml::Element* element, Rml::PropertyId id, const Rml::Property& local, InspectStyle& style){
            const Rml::PropertyMap& inlineProperties = element->GetLocalStyleProperties();
            if (inlineProperties.find(id) != inlineProperties.end()){
                styleAttribute(element, style);
                return;
            }
            locateSource(element, local, Rml::StyleSheetSpecification::GetPropertyName(id), id, style);
        }

        void styleAttribute(Rml::Element* element, InspectStyle& style){
            if (doc.compiled){
                if (std::optional<hc::Diagnostic> location = hc::locateElement(*doc.compiled, element)){
                    setLocation(style, location->file, location->line, 0, " (style attribute)");
                    return;
                }
            }
            setLocation(style, doc.path, 0, 0, " (style attribute)");
        }

        void locateSource(Rml::Element* element, const Rml::Property& property, const std::string& rmlName, Rml::PropertyId id, InspectStyle& style){
            if (property.origin == Rml::StyleOrigin::UserAgent){
                style.source = "user-agent";
                return;
            }
            const Rml::PropertySource* source = property.source.get();
            if (!source){
                // set without a sheet (a script's style, an animation)
                styleAttribute(element, style);
                return;
            }
            if (!sheets){
                // an .rml document: RmlUi's own location
                setLocation(style, hc::RmlLogTranslator::normalizeUrl(source->path), (uint32_t)std::max(0, source->line_number), 0);
                style.selector = source->rule_name;
                return;
            }
            const SheetMap::Sheet* sheet = sheets->find(source->path, source->line_number);
            if (!sheet){
                setLocation(style, hc::RmlLogTranslator::normalizeUrl(source->path), (uint32_t)std::max(0, source->line_number), 0);
                style.selector = source->rule_name;
                return;
            }
            // the rule's first line ("<selectors> {"), then its declaration of the property
            size_t rule = (size_t)(source->line_number - (int)sheet->firstLine);
            for (size_t back = 0; back < 3 && rule > 0 && !endsWithBrace(sheet->lines[rule]); back++)
                rule--;
            size_t declaration = rule;
            for (size_t i = rule + 1; i < sheet->lines.size(); i++){
                const std::string line = trim(sheet->lines[i]);
                if (!line.empty() && (line[0] == '}' || line.back() == '{'))
                    break;
                const size_t colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                const std::string name = trim(std::string_view(line).substr(0, colon));
                if ((!rmlName.empty() && id != Rml::PropertyId::Invalid && declares(name, id)) || (rmlName.empty() && name == style.property))
                    declaration = i;
            }
            const hc::SourceRefList& refs = sheet->output->lines.lines;
            const hc::SourceRef ref = declaration < refs.size() ? refs[declaration] : hc::SourceRef();
            const hc::SourceFiles& files = doc.compiled->files;
            if (ref.valid()){
                const hc::SourcePos position = files.position(ref.file, ref.offset);
                setLocation(style, files.path(ref.file), position.line, position.column);
            }else{
                // a generated rule: the sheet it is in
                setLocation(style, sheet->output->sourcePath.empty() ? doc.path : sheet->output->sourcePath, 0, 0);
            }
            if (sheet->hints)
                return;
            const hc::SourceRef ruleRef = rule < refs.size() ? refs[rule] : hc::SourceRef();
            const std::string prelude = ruleRef.valid() ? preludeOf(files.slice(ruleRef)) : std::string();
            if (prelude.empty()){
                style.selector = source->rule_name;
                return;
            }
            style.selector = prelude;
            const std::vector<std::string> pieces = splitSelectors(prelude);
            if (pieces.size() < 2)
                return;
            for (const std::string& piece : pieces){
                std::string rml, error;
                if (rmlSelector(doc, piece, rml, error) && matches(element, rml)){
                    style.selector = piece;
                    return;
                }
            }
        }

        const Doc& doc;
        Refs& refs;
        std::unique_ptr<SheetMap> sheets;
    };

    // the value of a shorthand: its longhands' values, one when they agree
    std::string shorthandText(Rml::Element* element, Rml::ShorthandId shorthand, Rml::PropertyId* first){
        std::vector<std::string> values;
        for (Rml::PropertyId id : Rml::StyleSheetSpecification::GetShorthandUnderlyingProperties(shorthand)){
            if (first && values.empty())
                *first = id;
            values.push_back(valueText(element, id));
        }
        bool same = true;
        for (const std::string& value : values)
            same = same && value == values.front();
        if (values.empty())
            return std::string();
        if (same)
            return values.front();
        std::string text;
        for (const std::string& value : values)
            text += (text.empty() ? "" : " ") + value;
        return text;
    }

    void computeStyle(Rml::Element* element, const StyleTarget& target, StyleSources* sources, InspectStyle& style){
        style = InspectStyle();
        style.property = target.css;
        switch (target.kind){
            case StyleTarget::Kind::Longhand:
                style.value = valueText(element, target.id);
                if (sources)
                    sources->locate(element, target.id, style);
                break;
            case StyleTarget::Kind::Shorthand:{
                Rml::PropertyId first = Rml::PropertyId::Invalid;
                style.value = shorthandText(element, target.shorthand, &first);
                if (sources && first != Rml::PropertyId::Invalid)
                    sources->locate(element, first, style);
                break;
            }
            case StyleTarget::Kind::Custom:{
                const Rml::Property* property = element->GetStyle()->GetSpecifiedCustomProperty(target.css);
                style.value = property ? trim(property->ToString()) : std::string();
                if (sources)
                    sources->locateCustom(element, target.css, style);
                break;
            }
            case StyleTarget::Kind::Unsupported:
                style.source = "not supported";
                break;
        }
    }

    bool resolveTargets(const std::vector<std::string>& properties, std::vector<StyleTarget>& targets, std::string& error){
        targets.clear();
        for (const std::string& name : properties){
            StyleTarget target;
            if (!resolveTarget(name, target, error))
                return false;
            targets.push_back(target);
        }
        return true;
    }

    // properties empty: the always-shown ones, then every longhand whose value is not its initial one
    void defaultTargets(Rml::Element* element, std::vector<StyleTarget>& targets){
        static const char* const always[] = {"display", "position", "width", "height", "color", "font-size", "font-family"};
        std::set<std::string> seen;
        for (const char* name : always){
            StyleTarget target;
            std::string error;
            if (resolveTarget(name, target, error)){
                targets.push_back(target);
                seen.insert(target.css);
            }
        }
        for (const auto& entry : propertyNames().sorted){
            if (seen.count(entry.first))
                continue;
            const Rml::PropertyDefinition* definition = Rml::StyleSheetSpecification::GetProperty(entry.second);
            const Rml::Property* value = element->GetProperty(entry.second);
            if (!definition || !value)
                continue;
            const Rml::Property* initial = definition->GetDefaultValue();
            if (initial && *initial == *value)
                continue;
            StyleTarget target;
            target.kind = StyleTarget::Kind::Longhand;
            target.css = entry.first;
            target.id = entry.second;
            targets.push_back(target);
            seen.insert(entry.first);
        }
    }

    // the first element of a selector, or an error naming it
    bool first(const Doc& doc, const std::string& selector, Rml::Element*& out, std::string& error){
        std::vector<Rml::Element*> found;
        if (!select(doc, selector, found, error))
            return false;
        if (found.empty()){
            error = "selector \"" + selector + "\" matched nothing";
            return false;
        }
        out = found.front();
        return true;
    }

    // context(): the context to lay out, found again when restoring: fn may remove a
    // texture-mode document (its context goes with it) or reload one
    bool withContext(const std::function<HtmlUIContext*()>& context, float width, float height, const std::function<void()>& fn, std::string& error){
        HtmlUIContext* target = context();
        if (!target || !target->getRmlContext()){
            error = "the scene has no HTML UI context (no UI Document is loaded)";
            return false;
        }
        if (!std::isfinite(width) || !std::isfinite(height) || width < 1.0f || height < 1.0f || width > 16384.0f || height > 16384.0f){
            error = "viewport " + number(width) + "x" + number(height) + " is not valid (1 to 16384 px each)";
            return false;
        }
        // the transforms follow the layout (rendering computes them, and nothing renders in between: fork P4-6)
        const auto layOut = [](HtmlUIContext* laidOut, Rml::Vector2i dimensions){
            laidOut->setDimensions(dimensions);
            laidOut->update();
            if (Rml::Element* root = laidOut->getRmlContext() ? laidOut->getRmlContext()->GetRootElement() : nullptr)
                root->UpdateTransformStates();
        };
        // restores the dimensions even when fn throws
        struct Restore {
            const std::function<HtmlUIContext*()>& context;
            HtmlUIContext* target;
            Rml::Vector2i dimensions;
            void (*layOut)(HtmlUIContext*, Rml::Vector2i);
            ~Restore(){
                if (context() != target)
                    return;
                layOut(target, dimensions);
            }
        } restore{context, target, target->getDimensions(), layOut};
        layOut(target, Rml::Vector2i((int)std::lround(width), (int)std::lround(height)));
        if (fn)
            fn();
        return true;
    }

}

// ---- inspect:: (shared with HtmlUIAssertions) ----

bool inspect::open(Scene* scene, Entity entity, Doc& doc, std::string& error){
    doc = Doc();
    if (!scene){
        error = "no scene";
        return false;
    }
    UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
    if (!component){
        error = "entity " + std::to_string(entity) + " has no UI Document component";
        return false;
    }
    std::shared_ptr<UIDocumentSystem> system = scene->getSystem<UIDocumentSystem>();
    Rml::ElementDocument* document = system ? system->getDocument(entity) : nullptr;
    if (!document){
        error = component->filename.empty() ? std::string("the UI Document has no file") : "the document " + component->filename + " is not loaded";
        return false;
    }
    doc.scene = scene;
    doc.entity = entity;
    doc.system = system.get();
    doc.document = document;
    doc.path = component->filename;
    doc.context = system->getTextureContext(entity);
    if (!doc.context)
        doc.context = system->getContext();
    doc.compat = hc::LuaHooks::isCompat(document);
    doc.compiled = system->getCompiledDocument(entity);
    document->UpdateDocument();
    doc.body = doc.compat ? hc::LuaHooks::body(document) : nullptr;
    if (!doc.body)
        doc.body = document;
    const Rml::Vector2i dimensions = doc.context ? doc.context->getDimensions()
                                                 : (document->GetContext() ? document->GetContext()->GetDimensions() : Rml::Vector2i(0, 0));
    doc.width = (float)dimensions.x;
    doc.height = (float)dimensions.y;
    return true;
}

bool inspect::rmlSelector(const Doc& doc, const std::string& selector, std::string& out, std::string& error){
    const std::string text = trim(selector);
    if (text.empty()){
        error = "selector is empty";
        return false;
    }
    const std::string pseudoElement = pseudoElementOf(text);
    if (!pseudoElement.empty()){
        error = invalidSelector(text, pseudoElement + " is a pseudo-element, not an element: select the element it belongs to");
        return false;
    }
    if (!doc.compat){
        out = text;
        return true;
    }
    hc::SourceFiles files;
    hc::DiagnosticSink sink(files);
    out = hc::CssRewriter::rewriteSelector(text, &sink);
    std::string problem;
    for (const hc::Diagnostic& diagnostic : sink.diagnostics()){
        if (diagnostic.severity == hc::Severity::Error){
            problem = diagnostic.message;
            break;
        }
    }
    if (problem.empty() && out.empty())
        problem = sink.diagnostics().empty() ? std::string("it selects nothing the engine supports") : sink.diagnostics().front().message;
    if (!problem.empty()){
        error = invalidSelector(text, problem);
        return false;
    }
    return true;
}

bool inspect::select(const Doc& doc, const std::string& selector, std::vector<Rml::Element*>& out, std::string& error){
    out.clear();
    if (trim(selector).empty()){
        out.push_back(doc.body);
        return true;
    }
    std::string rml;
    if (!rmlSelector(doc, selector, rml, error))
        return false;

    hc::RmlLogTranslator::Capture capture;
    Rml::ElementList found;
    const bool root = doc.document->Matches(rml);
    doc.document->QuerySelectorAll(found, rml);
    if (capture.problems() > 0 && found.empty() && !root){
        error = invalidSelector(trim(selector), capture.messages().front().text);
        return false;
    }
    if (root)
        out.push_back(doc.document);
    for (Rml::Element* element : found){
        if (isElement(element) && !hasSvgAncestor(element))
            out.push_back(element);
    }
    return true;
}

// no text node, and no data-for template (the hidden element RmlUi keeps after the rows, which the
// Lua DOM skips too, UIElementAccess::isDataForTemplate)
bool inspect::isElement(const Rml::Element* element){
    return element && !rmlui_dynamic_cast<const Rml::ElementText*>(element) && !UIElementAccess::isDataForTemplate(element);
}

bool inspect::isLeaf(const Rml::Element* element){
    return element->GetTagName() == "svg";
}

void inspect::elementChildren(Rml::Element* element, std::vector<Rml::Element*>& out){
    out.clear();
    if (isLeaf(element))
        return;
    const int count = element->GetNumChildren();
    for (int i = 0; i < count; i++){
        Rml::Element* child = element->GetChild(i);
        if (isElement(child))
            out.push_back(child);
    }
}

bool inspect::isInclusiveAncestor(const Rml::Element* ancestor, const Rml::Element* element){
    for (const Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
        if (walk == ancestor)
            return true;
    }
    return false;
}

std::string inspect::tagOf(const Doc& doc, const Rml::Element* element){
    if (doc.compat)
        return hc::LuaHooks::localName(element);
    return element->GetTagName();
}

// ---- refs ----

inspect::Refs::Refs(const Doc& doc): doc(doc){
    std::vector<Rml::Element*> stack{doc.document};
    std::vector<Rml::Element*> children;
    while (!stack.empty()){
        Rml::Element* element = stack.back();
        stack.pop_back();
        const std::string& id = element->GetId();
        if (!id.empty())
            idCounts[id]++;
        elementChildren(element, children);
        stack.insert(stack.end(), children.begin(), children.end());
    }
}

bool inspect::Refs::uniqueId(Rml::Element* element){
    const std::string& id = element->GetId();
    if (!isPlainIdent(id))
        return false;
    if (doc.compat && element == doc.document)
        return false;
    auto it = idCounts.find(id);
    return it != idCounts.end() && it->second == 1;
}

// whether "tag" selects the element by its DOM name (an element whose RML tag the selector
// rewrite names differently is stepped to by :nth-child alone)
bool inspect::Refs::tagSelects(Rml::Element* element){
    const std::string& rmlTag = element->GetTagName();
    auto it = tagChecks.find(rmlTag);
    if (it != tagChecks.end())
        return it->second;
    const std::string tag = tagOf(doc, element);
    std::string rml, error;
    const bool ok = isPlainIdent(tag) && rmlSelector(doc, tag, rml, error) && matches(element, rml);
    tagChecks[rmlTag] = ok;
    return ok;
}

std::string inspect::Refs::of(Rml::Element* element){
    auto cached = cache.find(element);
    if (cached != cache.end())
        return cached->second;
    std::string ref;
    if (element == doc.document){
        ref = doc.compat ? "html" : "body";
    }else if (element == doc.body){
        ref = "body";
    }else if (uniqueId(element)){
        ref = "#" + element->GetId();
    }else{
        std::vector<std::string> steps;
        std::string anchor;
        Rml::Element* walk = element;
        while (walk){
            Rml::Element* parent = walk->GetParentNode();
            if (!parent){
                anchor = tagOf(doc, walk);
                break;
            }
            int index = 1;
            const int count = parent->GetNumChildren();
            for (int i = 0; i < count; i++){
                Rml::Element* sibling = parent->GetChild(i);
                if (sibling == walk)
                    break;
                if (isElement(sibling))
                    index++;
            }
            steps.push_back((tagSelects(walk) ? tagOf(doc, walk) : std::string()) + ":nth-child(" + std::to_string(index) + ")");
            walk = parent;
            if (walk == doc.body){
                anchor = "body";
                break;
            }
            if (walk == doc.document){
                anchor = doc.compat ? "html" : "body";
                break;
            }
            if (uniqueId(walk)){
                anchor = "#" + walk->GetId();
                break;
            }
        }
        ref = anchor;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it)
            ref += " > " + *it;
    }
    cache[element] = ref;
    return ref;
}

// ---- geometry ----

InspectRect inspect::boxRect(Rml::Element* element, Rml::BoxArea area){
    if (!hasLayoutBox(element))
        return InspectRect{0, 0, 0, 0};
    const Rml::Vector2f position = element->GetAbsoluteOffset(area);
    const Rml::Vector2f size = element->GetBox().GetSize(area);
    return transformed(element, InspectRect{position.x, position.y, size.x, size.y});
}

InspectRect inspect::borderRect(Rml::Element* element){
    if (!hasLayoutBox(element))
        return InspectRect{0, 0, 0, 0};
    const Rml::Vector2f origin = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
    InspectRect rect{origin.x, origin.y, size.x, size.y};
    const int boxes = element->GetNumBoxes();
    for (int i = 1; i < boxes; i++){
        Rml::Vector2f offset;
        const Rml::Box& box = element->GetBox(i, offset);
        const Rml::Vector2f boxSize = box.GetSize(Rml::BoxArea::Border);
        rect = unite(rect, InspectRect{origin.x + offset.x, origin.y + offset.y, boxSize.x, boxSize.y});
    }
    return transformed(element, rect);
}

InspectRect inspect::intersect(const InspectRect& a, const InspectRect& b){
    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    const float right = std::min(a.x + a.width, b.x + b.width);
    const float bottom = std::min(a.y + a.height, b.y + b.height);
    return InspectRect{left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
}

InspectRect inspect::unite(const InspectRect& a, const InspectRect& b){
    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x + a.width, b.x + b.width);
    const float bottom = std::max(a.y + a.height, b.y + b.height);
    return InspectRect{left, top, right - left, bottom - top};
}

bool inspect::isEmpty(const InspectRect& rect){
    return !(rect.width > 0.0f) || !(rect.height > 0.0f);
}

bool inspect::contains(const InspectRect& outer, const InspectRect& inner, float tolerance){
    return inner.x >= outer.x - tolerance && inner.y >= outer.y - tolerance &&
           inner.x + inner.width <= outer.x + outer.width + tolerance &&
           inner.y + inner.height <= outer.y + outer.height + tolerance;
}

InspectRect inspect::transformedRect(Rml::Element* element, const InspectRect& rect){
    return transformed(element, rect);
}

inspect::ClipRegion inspect::clipOf(Rml::Element* element, const std::function<bool(Rml::Element*)>& skip){
    using Rml::Style::Clip;
    using Rml::Style::Overflow;
    ClipRegion region;
    const Clip ownClip = element->GetComputedValues().clip();
    if (ownClip == Clip::Type::None)
        return region;
    int ignored = ownClip.GetNumber();

    for (Rml::Element* clipping = element->GetOffsetParent(); clipping; clipping = clipping->GetOffsetParent()){
        const Rml::Style::ComputedValues& computed = clipping->GetComputedValues();
        const Overflow overflowX = computed.overflow_x(), overflowY = computed.overflow_y();
        const bool enabled = overflowX != Overflow::Visible || overflowY != Overflow::Visible;
        const bool always = computed.clip() == Clip::Type::Always;
        const bool none = computed.clip() == Clip::Type::None;

        if ((always || enabled) && ignored == 0 && !(skip && skip(clipping))){
            const Rml::BoxArea area = clipping->GetClipArea();
            InspectRect rect = boxRect(clipping, area);
            // a scroll container clips to its scrollport: the padding box without the scrollbars
            if (area == Rml::BoxArea::Padding){
                Rml::ElementScroll* scroll = clipping->GetElementScroll();
                rect.width = std::max(0.0f, rect.width - scroll->GetScrollbarSize(Rml::ElementScroll::VERTICAL));
                rect.height = std::max(0.0f, rect.height - scroll->GetScrollbarSize(Rml::ElementScroll::HORIZONTAL));
            }
            // 'overflow: clip' clips its axis only, extended by 'overflow-clip-margin'
            if (!always && (overflowX == Overflow::Clip || overflowY == Overflow::Clip)){
                constexpr float unbounded = 1e9f;
                const float margin = computed.overflow_clip_margin();
                if (overflowX == Overflow::Clip){
                    rect.x -= margin;
                    rect.width += 2 * margin;
                }else{
                    rect.x = -unbounded;
                    rect.width = 2 * unbounded;
                }
                if (overflowY == Overflow::Clip){
                    rect.y -= margin;
                    rect.height += 2 * margin;
                }else{
                    rect.y = -unbounded;
                    rect.height = 2 * unbounded;
                }
            }
            region.rect = region.bounded ? intersect(region.rect, rect) : rect;
            region.bounded = true;
            region.clippers.emplace_back(clipping, rect);
        }

        if (ignored > 0 && enabled)
            ignored--;
        ignored = std::max(ignored, computed.clip().GetNumber());
        if (none)
            break;
    }
    return region;
}

Rml::Element* inspect::clipperCutting(const ClipRegion& clip, const InspectRect& rect, float tolerance){
    for (const auto& clipper : clip.clippers){
        if (!contains(clipper.second, rect, tolerance))
            return clipper.first;
    }
    return nullptr;
}

Rml::Element* inspect::displayNoneAt(Rml::Element* element){
    for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
        if (walk->GetComputedValues().display() == Rml::Style::Display::None)
            return walk;
        if (rmlui_dynamic_cast<Rml::ElementDocument*>(walk))
            break;
    }
    return nullptr;
}

bool inspect::hasLayoutBox(Rml::Element* element){
    if (!element || element->GetComputedValues().display() == Rml::Style::Display::Contents)
        return false;
    for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
        if (walk->GetComputedValues().display() == Rml::Style::Display::None)
            return false;
        if (rmlui_dynamic_cast<Rml::ElementDocument*>(walk))
            return true;
    }
    // not in a document
    return false;
}

float inspect::effectiveOpacity(Rml::Element* element){
    float opacity = 1.0f;
    for (Rml::Element* walk = element; walk; walk = walk->GetParentNode()){
        opacity *= walk->GetComputedValues().opacity();
        if (rmlui_dynamic_cast<Rml::ElementDocument*>(walk))
            break;
    }
    return opacity;
}

InspectFlags inspect::flagsOf(const Doc& doc, Rml::Element* element){
    InspectFlags flags;
    flags.transparent = effectiveOpacity(element) < 0.05f;
    if (displayNoneAt(element)){
        // not laid out: no geometry to judge
        flags.hidden = true;
        return flags;
    }
    flags.hidden = element->GetComputedValues().visibility() != Rml::Style::Visibility::Visible;
    flags.overflowX = element->GetScrollWidth() > element->GetClientWidth() + 0.5f;
    flags.overflowY = element->GetScrollHeight() > element->GetClientHeight() + 0.5f;
    const InspectRect border = borderRect(element);
    flags.zeroSize = border.width < 1.0f || border.height < 1.0f;
    flags.offscreen = border.x + border.width <= 0.0f || border.y + border.height <= 0.0f || border.x >= doc.width || border.y >= doc.height;
    const ClipRegion clip = clipOf(element);
    flags.clipped = clip.bounded && !contains(clip.rect, border, 0.5f);
    return flags;
}

std::string inspect::ownText(Rml::Element* element){
    if (isLeaf(element))
        return std::string();
    std::string text;
    const int count = element->GetNumChildren();
    for (int i = 0; i < count; i++){
        if (Rml::ElementText* child = rmlui_dynamic_cast<Rml::ElementText*>(element->GetChild(i))){
            text += ' ';
            text += child->GetText();
        }
    }
    return collapse(Rml::StringUtilities::DecodeRml(text));
}

InspectElement inspect::describe(const Doc& doc, Refs& refs, Rml::Element* element){
    InspectElement out;
    out.ref = refs.of(element);
    out.tag = tagOf(doc, element);
    if (!(doc.compat && element == doc.document))
        out.id = element->GetId();
    std::vector<std::string> classes;
    Rml::StringUtilities::ExpandString(classes, element->GetClassNames(), ' ');
    for (const std::string& name : classes){
        if (!name.empty() && !(doc.compat && hc::LuaHooks::isHiddenClass(name)))
            out.classes.push_back(name);
    }
    for (const char* pseudo : kPseudoClasses){
        bool set = element->IsPseudoClassSet(pseudo);
        if (!set && (std::strcmp(pseudo, "checked") == 0 || std::strcmp(pseudo, "disabled") == 0)){
            // structural in the fork (attributes, a disabled fieldset): matched, not set
            std::string rml, error;
            set = rmlSelector(doc, std::string(":") + pseudo, rml, error) && matches(element, rml);
        }
        if (set)
            out.pseudo.push_back(pseudo);
    }
    out.text = truncateUtf8(ownText(element), kTextLimit);
    out.border = borderRect(element);
    out.opacity = effectiveOpacity(element);
    if (!isLeaf(element)){
        const int count = element->GetNumChildren();
        for (int i = 0; i < count; i++){
            if (isElement(element->GetChild(i)))
                out.childCount++;
        }
    }
    out.flags = flagsOf(doc, element);
    std::map<std::string, int> ordinals;
    for (EffectUse& use : collectEffectUses(element)){
        InspectEffect effect;
        effect.property = use.property;
        effect.index = use.index;
        effect.name = use.name;
        effect.form = use.filterForm ? "filter" : "paint";
        effect.params = std::move(use.params);
        // phase 6: the n-th use of a stateful effect's name is its n-th simulation
        const EffectManifest* manifest = EffectRegistry::get().find(use.name);
        if (manifest && manifest->stateful()){
            const EffectSimulationInfo sim = EffectRuntime::get().simulationInfo(element, use.name, ordinals[use.name]++);
            effect.stateful = true;
            effect.stateBytes = sim.bytes;
            effect.steps = sim.steps;
            effect.simTime = sim.time;
            effect.paused = sim.paused;
            effect.stateImages = sim.images;
            effect.replays = sim.replays;
            effect.replaying = sim.replaying;
            effect.splats = sim.splats;
            effect.pathSamples = sim.pathSamples;
        }
        out.effects.push_back(std::move(effect));
    }
    if (UIVideoElement* video = rmlui_dynamic_cast<UIVideoElement*>(element)){
        auto media = std::make_shared<InspectMedia>();
        media->currentSrc = video->getCurrentSrc();
        media->backend = video->getVideoBackend();
        media->currentTime = video->getCurrentTime();
        const double duration = video->getDuration();
        media->duration = std::isfinite(duration) ? duration : -1.0;
        media->playbackRate = video->getPlaybackRate();
        media->paused = video->isPaused();
        media->ended = video->isEnded();
        media->seeking = video->isSeeking();
        media->loop = video->getLoop();
        media->muted = video->isMuted();
        media->showingPoster = video->isShowingPoster();
        media->volume = video->getVolume();
        media->readyState = video->getReadyState();
        media->networkState = video->getNetworkState();
        media->videoWidth = video->getVideoWidth();
        media->videoHeight = video->getVideoHeight();
        media->errorCode = (int)video->getError().code;
        media->errorMessage = video->getError().message;
        media->frame = video->getShownFrameIndex();
        media->droppedFrames = video->getStreamStats().framesDropped;
        media->decodeMs = video->getDecodeMsPerFrame();
        media->memoryBytes = video->getMemoryBytes();
        media->controls = video->getControls();
        if (UIVideoControls* controls = video->getControlsWidget()){
            const UIVideoControls::Rects rects = controls->getRects();
            auto toRect = [](const Rml::Rectanglef& r){
                InspectRect out;
                out.x = r.Left();
                out.y = r.Top();
                out.width = r.Width();
                out.height = r.Height();
                return out;
            };
            media->controlsVisible = rects.visible;
            media->controlsBar = toRect(rects.bar);
            media->playButton = toRect(rects.play);
            media->timeline = toRect(rects.timeline);
            media->muteButton = toRect(rects.mute);
        }
        out.media = std::move(media);
    }
    return out;
}

// ---- hit test ----

namespace {

    // a child the DOM lists (not a scrollbar, pseudo-element or other part RmlUi appends)
    bool isDomElement(Rml::Element* element){
        if (!isElement(element))
            return false;
        Rml::Element* parent = element->GetParentNode();
        if (!parent)
            return true;
        const int count = parent->GetNumChildren();
        for (int i = 0; i < count; i++){
            if (parent->GetChild(i) == element)
                return true;
        }
        return false;
    }


}

Rml::Element* inspect::hitAt(const Doc& doc, float x, float y, bool wholeContext){
    Rml::Context* context = doc.document->GetContext();
    if (!context)
        return nullptr;
    // (the context skips the pages the pointer passes through: HtmlUIContext::passesPointerThrough)
    Rml::Element* hit = wholeContext ? context->GetElementAtPoint(Rml::Vector2f(x, y))
                                     : context->GetElementAtPoint(Rml::Vector2f(x, y), nullptr, doc.document);
    while (hit && !isDomElement(hit))
        hit = hit->GetParentNode();
    if (!hit || hit == context->GetRootElement() || HtmlUIContext::passesPointerThrough(hit))
        return nullptr;
    return hit;
}

void inspect::hitStack(const Doc& doc, Rml::Element* hit, std::vector<Rml::Element*>& out){
    out.clear();
    for (Rml::Element* walk = hit; walk; walk = walk->GetParentNode()){
        out.push_back(walk);
        if (walk == doc.body || walk == doc.document)
            break;
    }
}

std::string inspect::number(double value, int decimals){
    if (!std::isfinite(value))
        return "0";
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text = buffer;
    if (text.find('.') != std::string::npos){
        while (!text.empty() && text.back() == '0')
            text.pop_back();
        if (!text.empty() && text.back() == '.')
            text.pop_back();
    }
    if (text == "-0")
        text = "0";
    return text;
}

// ---- HtmlUIInspector ----

std::vector<Entity> HtmlUIInspector::documents(Scene* scene){
    std::vector<Entity> entities;
    if (!scene)
        return entities;
    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++)
        entities.push_back(components->getEntity(i));
    std::sort(entities.begin(), entities.end());
    return entities;
}

bool HtmlUIInspector::documentInfo(Scene* scene, Entity entity, DocumentInfo& out, std::string& error){
    out = DocumentInfo();
    if (!scene){
        error = "no scene";
        return false;
    }
    UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
    if (!component){
        error = "entity " + std::to_string(entity) + " has no UI Document component";
        return false;
    }
    std::shared_ptr<UIDocumentSystem> system = scene->getSystem<UIDocumentSystem>();
    out.entity = entity;
    out.html = component->filename;
    out.renderToTexture = component->renderMode == UIDocumentRenderMode::TEXTURE;
    out.visible = component->enabled;
    out.sortOrder = component->sortOrder;
    if (!system)
        return true;
    out.loaded = system->isDocumentLoaded(entity);
    HtmlUIContext* context = system->getTextureContext(entity);
    if (!context)
        context = system->getContext();
    if (context){
        const Rml::Vector2i dimensions = context->getDimensions();
        out.width = (float)dimensions.x;
        out.height = (float)dimensions.y;
    }
    out.loadCount = system->getLoadCount(entity);
    out.diagnostics = system->getDiagnostics(entity);
    out.dependencies = system->getDocumentFiles(entity);
    out.dataModels = system->getDataModelNames(entity);
    return true;
}

bool HtmlUIInspector::query(Scene* scene, Entity entity, const std::string& selector, int depth, size_t limit,
                            std::vector<InspectElement>& out, bool& truncated, std::string& error){
    out.clear();
    truncated = false;
    Doc doc;
    if (!open(scene, entity, doc, error))
        return false;
    std::vector<Rml::Element*> found;
    if (!select(doc, selector, found, error))
        return false;
    const std::unordered_set<Rml::Element*> matchSet(found.begin(), found.end());
    depth = std::max(0, depth);
    Refs refs(doc);

    // document order; `levels` = descendant levels still listed below the nearest listed match
    struct Frame {
        Rml::Element* element;
        int parent;        // index of the parent element in `out`, -1 when not listed
        int ancestor;      // index of the nearest listed ancestor, -1 when none
        int levels;
    };
    std::vector<Frame> stack{Frame{doc.document, -1, -1, 0}};
    std::vector<Rml::Element*> children;
    while (!stack.empty()){
        const Frame frame = stack.back();
        stack.pop_back();
        const bool match = matchSet.count(frame.element) > 0;
        const bool listed = match || frame.levels > 0;
        int index = -1;
        int levels = 0;
        if (listed){
            if (out.size() >= limit){
                truncated = true;
                break;
            }
            InspectElement element = describe(doc, refs, frame.element);
            element.index = (int)out.size();
            element.parent = frame.parent;
            element.depth = frame.ancestor >= 0 ? out[frame.ancestor].depth + 1 : 0;
            index = element.index;
            out.push_back(std::move(element));
            levels = match ? std::max(depth, frame.levels - 1) : frame.levels - 1;
        }
        elementChildren(frame.element, children);
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            stack.push_back(Frame{*it, index, listed ? index : frame.ancestor, levels});
    }
    return true;
}

bool HtmlUIInspector::hitTest(Scene* scene, Entity entity, float x, float y, std::vector<InspectElement>& stack, std::string& error){
    stack.clear();
    Doc doc;
    if (!open(scene, entity, doc, error))
        return false;
    std::vector<Rml::Element*> elements;
    if (Rml::Element* hit = hitAt(doc, x, y, false))
        hitStack(doc, hit, elements);
    Refs refs(doc);
    for (size_t i = 0; i < elements.size(); i++){
        InspectElement element = describe(doc, refs, elements[i]);
        element.index = (int)i;
        element.parent = i + 1 < elements.size() ? (int)i + 1 : -1;
        element.depth = (int)(elements.size() - 1 - i);
        stack.push_back(std::move(element));
    }
    return true;
}

bool HtmlUIInspector::detail(Scene* scene, Entity entity, const std::string& ref, const std::vector<std::string>& properties,
                             InspectDetail& out, std::string& error){
    out = InspectDetail();
    Doc doc;
    if (!open(scene, entity, doc, error))
        return false;
    std::vector<StyleTarget> targets;
    if (!resolveTargets(properties, targets, error))
        return false;
    Rml::Element* element = nullptr;
    if (!first(doc, ref, element, error))
        return false;

    Refs refs(doc);
    out.element = describe(doc, refs, element);
    out.element.index = 0;

    InspectBoxModel& box = out.box;
    box.content = boxRect(element, Rml::BoxArea::Content);
    box.padding = boxRect(element, Rml::BoxArea::Padding);
    box.border = boxRect(element, Rml::BoxArea::Border);
    box.margin = boxRect(element, Rml::BoxArea::Margin);
    // no box (display: none here or above): every size is 0, as the browser's APIs report it
    if (hasLayoutBox(element)){
        const Rml::Box& layout = element->GetBox();
        const auto edges = [&layout](Rml::BoxArea area){
            return InspectEdges{layout.GetEdge(area, Rml::BoxEdge::Top), layout.GetEdge(area, Rml::BoxEdge::Right),
                                layout.GetEdge(area, Rml::BoxEdge::Bottom), layout.GetEdge(area, Rml::BoxEdge::Left)};
        };
        box.paddingEdges = edges(Rml::BoxArea::Padding);
        box.borderEdges = edges(Rml::BoxArea::Border);
        box.marginEdges = edges(Rml::BoxArea::Margin);
        box.scrollWidth = element->GetScrollWidth();
        box.scrollHeight = element->GetScrollHeight();
        box.clientWidth = element->GetClientWidth();
        box.clientHeight = element->GetClientHeight();
        box.scrollLeft = element->GetScrollLeft();
        box.scrollTop = element->GetScrollTop();
    }

    for (const auto& attribute : element->GetAttributes()){
        if (doc.compat && hc::LuaHooks::isHiddenAttribute(attribute.first))
            continue;
        std::string value = attribute.second.Get<Rml::String>();
        if (doc.compat && attribute.first == "class")
            value = hc::LuaHooks::visibleClassNames(value);
        if (doc.compat && element == doc.document && attribute.first == "id")
            continue;
        out.attributes.emplace_back(attribute.first, value);
    }
    std::sort(out.attributes.begin(), out.attributes.end());

    if (targets.empty())
        defaultTargets(element, targets);
    StyleSources sources(doc, refs);
    for (const StyleTarget& target : targets){
        InspectStyle style;
        computeStyle(element, target, &sources, style);
        out.styles.push_back(std::move(style));
    }
    return true;
}

bool HtmlUIInspector::styles(Scene* scene, Entity entity, const std::vector<std::string>& refs, const std::vector<std::string>& properties,
                             std::vector<std::vector<InspectStyle>>& out, std::string& error){
    out.clear();
    Doc doc;
    if (!open(scene, entity, doc, error))
        return false;
    std::vector<StyleTarget> targets;
    if (!resolveTargets(properties, targets, error))
        return false;
    for (const std::string& ref : refs){
        Rml::Element* element = nullptr;
        if (!first(doc, ref, element, error)){
            out.clear();
            return false;
        }
        std::vector<InspectStyle> values;
        for (const StyleTarget& target : targets){
            InspectStyle style;
            computeStyle(element, target, nullptr, style);
            values.push_back(std::move(style));
        }
        out.push_back(std::move(values));
    }
    return true;
}

bool HtmlUIInspector::elementRect(Scene* scene, Entity entity, const std::string& selector, InspectRect& out, std::string& error){
    out = InspectRect();
    Doc doc;
    if (!open(scene, entity, doc, error))
        return false;
    std::vector<Rml::Element*> found;
    if (!select(doc, selector, found, error))
        return false;
    if (found.empty()){
        error = "selector \"" + selector + "\" matched nothing";
        return false;
    }
    for (Rml::Element* element : found){
        const InspectFlags flags = flagsOf(doc, element);
        if (flags.hidden || flags.transparent || flags.zeroSize)
            continue;
        out = borderRect(element);
        return true;
    }
    error = "selector \"" + selector + "\" matched " + std::to_string(found.size()) + " element" + (found.size() == 1 ? "" : "s") +
            ", none visible (hidden, transparent or zero-size)";
    return false;
}

bool HtmlUIInspector::withViewport(Scene* scene, float width, float height, const std::function<void()>& fn, std::string& error){
    if (!scene){
        error = "no scene";
        return false;
    }
    std::shared_ptr<UIDocumentSystem> system = scene->getSystem<UIDocumentSystem>();
    return withContext([&system](){ return system ? system->getContext() : nullptr; }, width, height, fn, error);
}

bool HtmlUIInspector::withViewport(Scene* scene, Entity entity, float width, float height, const std::function<void()>& fn, std::string& error){
    if (!scene){
        error = "no scene";
        return false;
    }
    if (!scene->findComponent<UIDocumentComponent>(entity)){
        error = "entity " + std::to_string(entity) + " has no UI Document component";
        return false;
    }
    std::shared_ptr<UIDocumentSystem> system = scene->getSystem<UIDocumentSystem>();
    if (!system){
        error = "the scene has no HTML UI context (no UI Document is loaded)";
        return false;
    }
    // the context chosen now: a texture-mode document's own, else the scene's
    const bool texture = system->getTextureContext(entity) != nullptr;
    return withContext([&system, entity, texture](){ return texture ? system->getTextureContext(entity) : system->getContext(); },
                       width, height, fn, error);
}

bool HtmlUIInspector::pushDataModel(Scene* scene, Entity entity, const std::string& name, lua_State* L){
    if (!scene || !L)
        return false;
    std::shared_ptr<UIDocumentSystem> system = scene->getSystem<UIDocumentSystem>();
    return system && system->pushDataModel(L, entity, name);
}
