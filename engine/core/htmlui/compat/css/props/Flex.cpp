// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Flexbox and alignment (phase-2 §3.9.7 "Flexbox"): the flex shorthand keywords, alignment
// keywords RmlUi lacks, place-* shorthands, gaps, legacy -webkit-box-* / -ms-flex* names.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    void unsupportedValue(DeclContext& context, const std::string& suggestion = std::string()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.name + ": " + context.valueText() +
                         "' is not supported; declaration dropped", suggestion);
        context.dropped = true;
    }

    std::string basisText(const ComponentValue& v){
        if (v.isToken(TokenType::Number) && v.token.number == 0)
            return "0%";
        return text(v);
    }

    // the flex shorthand in RmlUi's three-value form
    std::optional<std::string> flexValue(Session& session, const std::vector<ComponentValue>& w){
        if (w.size() == 1){
            const std::string k = identOf(w[0]);
            if (k == "none")
                return std::string("0 0 auto");
            if (k == "auto")
                return std::string("1 1 auto");
            if (k == "initial")
                return std::string("0 1 auto");
            if (w[0].isToken(TokenType::Number))
                return text(w[0]) + " 1 0%";
            if (isLength(session, w[0], true) || k == "content")
                return "1 1 " + (k == "content" ? std::string("auto") : basisText(w[0]));
            return std::nullopt;
        }
        if (w.size() == 2 && w[0].isToken(TokenType::Number)){
            if (w[1].isToken(TokenType::Number) && w[1].token.number != 0)
                return text(w[0]) + " " + text(w[1]) + " 0%";
            if (isLength(session, w[1], true) || isIdent(w[1], "auto"))
                return text(w[0]) + " 1 " + basisText(w[1]);
            if (w[1].isToken(TokenType::Number))
                return text(w[0]) + " 0 0%";
            return std::nullopt;
        }
        if (w.size() == 3 && w[0].isToken(TokenType::Number) && w[1].isToken(TokenType::Number) &&
            (isLength(session, w[2], true) || isIdent(w[2], "auto") || isIdent(w[2], "content")))
            return text(w[0]) + " " + text(w[1]) + " " + (isIdent(w[2], "content") ? std::string("auto") : basisText(w[2]));
        return std::nullopt;
    }

    // an alignment keyword RmlUi knows; safe/unsafe dropped
    std::optional<std::string> alignKeyword(DeclContext& context, const std::string& property, const std::vector<ComponentValue>& w){
        std::vector<std::string> keywords;
        bool overflowPosition = false;
        for (const ComponentValue& v : w){
            const std::string k = identOf(v);
            if (k.empty())
                return std::nullopt;
            if (k == "safe" || k == "unsafe"){
                overflowPosition = true;
                continue;
            }
            keywords.push_back(k);
        }
        if (overflowPosition)
            context.reportAs(Severity::Info, codes::ApproximatedMinor, "'safe' / 'unsafe' are dropped from '" + property + "'");
        std::string keyword;
        for (const std::string& k : keywords)
            keyword += (keyword.empty() ? "" : " ") + k;
        if (keyword.empty())
            return std::nullopt;
        if (RcssGrammar::accepts(property, keyword))
            return keyword;
        if (const ValueSupport* row = findValue(property, keyword)){
            if (row->support == Support::Approximated)
                context.reportAs(Severity::Info, codes::ApproximatedMinor, "'" + property + ": " + keyword + "' is written as '" + row->output + "'");
            const std::string output = row->output;
            if (!output.empty() && RcssGrammar::accepts(property, output))
                return output;
        }
        // keywords the value table lists for a sibling property
        static const std::pair<const char*, const char*> common[] = {
            {"start", "flex-start"}, {"end", "flex-end"}, {"self-start", "flex-start"}, {"self-end", "flex-end"},
            {"left", "flex-start"}, {"right", "flex-end"}, {"first baseline", "baseline"}, {"last baseline", "baseline"},
        };
        for (const auto& c : common){
            if (keyword == c.first && RcssGrammar::accepts(property, c.second))
                return std::string(c.second);
        }
        return std::nullopt;
    }

}

void jasperengine::htmlui::compat::rewriteFlex(DeclContext& context){
    const std::string& n = context.name;
    std::vector<ComponentValue> w = words(context.value);
    if (w.size() == 1 && isVar(w[0])){
        context.emit(n == "-ms-flex" ? "flex" : n, text(w[0]));
        return;
    }
    if (n == "flex" || n == "-ms-flex"){
        std::optional<std::string> value = flexValue(context.session, w);
        if (!value){
            unsupportedValue(context, "use flex: <grow> <shrink> <basis>, none or auto");
            return;
        }
        context.emit("flex", *value);
        return;
    }
    // legacy flexbox names
    const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
    auto legacy = [&](const std::string& property, const std::string& value){
        context.reportAs(Severity::Info, codes::VendorPrefix, "'" + n + "' is read as '" + property + ": " + value + "'");
        if (!RcssGrammar::accepts(property, value)){
            unsupportedValue(context);
            return;
        }
        context.emit(property, value);
    };
    if (n == "-webkit-box-orient"){
        if (k == "horizontal" || k == "inline-axis")
            legacy("flex-direction", "row");
        else if (k == "vertical" || k == "block-axis")
            legacy("flex-direction", "column");
        else
            unsupportedValue(context);
        return;
    }
    if (n == "-webkit-box-direction"){
        if (k == "normal")
            legacy("flex-direction", "row");
        else if (k == "reverse")
            legacy("flex-direction", "row-reverse");
        else
            unsupportedValue(context);
        return;
    }
    if (n == "-webkit-box-pack" || n == "-ms-flex-pack"){
        const std::string value = k == "start" ? "flex-start" : k == "end" ? "flex-end" : k == "center" ? "center"
                                : k == "justify" ? "space-between" : k == "distribute" ? "space-around" : "";
        if (value.empty())
            unsupportedValue(context);
        else
            legacy("justify-content", value);
        return;
    }
    if (n == "-webkit-box-align" || n == "-ms-flex-align"){
        const std::string value = k == "start" ? "flex-start" : k == "end" ? "flex-end" : k;
        legacy("align-items", value);
        return;
    }
    if (n == "-webkit-box-flex" || n == "-ms-flex-positive"){
        legacy("flex-grow", text(w.empty() ? ComponentValue() : w[0]));
        return;
    }
    if (n == "-ms-flex-negative"){
        legacy("flex-shrink", text(w.empty() ? ComponentValue() : w[0]));
        return;
    }
    if (n == "-ms-flex-preferred-size"){
        legacy("flex-basis", w.empty() ? std::string() : basisText(w[0]));
        return;
    }
    if (n == "-ms-flex-direction"){
        legacy("flex-direction", k);
        return;
    }
    if (n == "-ms-flex-wrap"){
        legacy("flex-wrap", k == "none" ? "nowrap" : k);
        return;
    }
    unsupportedValue(context);
}

void jasperengine::htmlui::compat::rewriteAlignKeyword(DeclContext& context){
    const std::string& n = context.name;
    std::vector<ComponentValue> w = words(context.value);
    if (n == "justify-items" || n == "justify-self"){
        if (context.fork(ForkFeature::Grid)){
            context.emit(n, context.valueText());
            return;
        }
        context.reportAs(Severity::Info, codes::ForkFallback, "'" + n + "' has no effect in flex layout; ignored until the fork has Grid");
        context.dropped = true;
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        context.emit(n, text(w[0]));
        return;
    }
    std::optional<std::string> keyword = alignKeyword(context, n, w);
    if (!keyword){
        unsupportedValue(context, "use one of: " + RcssGrammar::keywords(n));
        return;
    }
    context.emit(n, *keyword);
}

void jasperengine::htmlui::compat::rewritePlaceAlign(DeclContext& context){
    const std::string& n = context.name;
    std::vector<ComponentValue> w = words(context.value);
    // "<align> [<justify>]": split where the second keyword group starts
    std::vector<ComponentValue> first, second;
    size_t i = 0;
    auto takeGroup = [&](std::vector<ComponentValue>& group){
        while (i < w.size()){
            const std::string k = identOf(w[i]);
            group.push_back(w[i]);
            i++;
            if (k != "safe" && k != "unsafe" && k != "first" && k != "last")
                break;
        }
    };
    takeGroup(first);
    takeGroup(second);
    if (first.empty() || i < w.size()){
        unsupportedValue(context);
        return;
    }
    if (second.empty())
        second = first;
    const std::string suffix = n.substr(6);   // content, items, self
    processSynthetic(context, "align-" + suffix, text(first));
    processSynthetic(context, "justify-" + suffix, text(second));
}

void jasperengine::htmlui::compat::rewriteGap(DeclContext& context){
    std::string n = context.name;
    if (n == "grid-gap")
        n = "gap";
    else if (n == "grid-row-gap")
        n = "row-gap";
    else if (n == "grid-column-gap")
        n = "column-gap";
    std::vector<std::string> values;
    for (const ComponentValue& v : words(context.value)){
        if (isIdent(v, "normal"))
            values.push_back("0px");
        else if (v.isToken(TokenType::Number))
            values.push_back(lengthText(v));
        else
            values.push_back(text(v));
    }
    const std::string value = join(values);
    if (!RcssGrammar::accepts(n, value)){
        unsupportedValue(context, "use a length or percentage");
        return;
    }
    context.emit(n, value);
}
