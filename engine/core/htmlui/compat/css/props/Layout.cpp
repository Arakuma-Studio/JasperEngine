// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Layout rewrites (phase-2 §3.9.7 "Layout"): display (the grid fallback while the fork lacks
// Grid), position, logical properties, sizing keywords, overflow, the grid properties.

#include "htmlui/compat/css/props/Rewriters.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssGrammar.h"
#include "htmlui/compat/css/Units.h"

#include <cmath>

using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::props;

namespace {

    void unsupportedValue(DeclContext& context, const std::string& suggestion = std::string()){
        context.reportAs(Severity::Warning, codes::UnsupportedValue, "'" + context.name + ": " + context.valueText() +
                         "' is not supported; declaration dropped", suggestion);
        context.dropped = true;
    }

    bool ruleDeclares(const DeclContext& context, std::initializer_list<const char*> names){
        if (!context.rule)
            return false;
        for (const Declaration& d : context.rule->declarations){
            for (const char* n : names){
                if (d.name == n)
                    return true;
            }
        }
        return false;
    }

    // ---- grid fallback: a track list to flex items

    struct Track {
        enum class Kind { Fixed, Fraction, Auto } kind = Kind::Auto;
        std::string length;   // Fixed: the RCSS length
        double fraction = 1;  // Fraction
    };

    std::optional<Track> trackOf(Session& session, const ComponentValue& v){
        Track track;
        if (v.isToken(TokenType::Dimension) && equalsNoCase(v.token.unit, "fr")){
            track.kind = Track::Kind::Fraction;
            track.fraction = v.token.number;
            return track;
        }
        if (isLength(session, v, true)){
            track.kind = Track::Kind::Fixed;
            track.length = lengthText(v);
            return track;
        }
        const std::string k = identOf(v);
        if (k == "auto" || k == "min-content" || k == "max-content")
            return track;
        if (v.isFunction("minmax")){
            std::vector<ValueList> args = commaGroups(v.children);
            if (args.size() == 2 && args[1].size() == 1)
                return trackOf(session, args[1][0]);
            return std::nullopt;
        }
        if (v.isFunction("fit-content")){
            ValueList w = words(v.children);
            if (w.size() == 1 && isLength(session, w[0], true)){
                track.kind = Track::Kind::Fixed;
                track.length = lengthText(w[0]);
                return track;
            }
        }
        return std::nullopt;
    }

    // the explicit columns of grid-template-columns; autoTrack set for repeat(auto-fill|auto-fit, ...)
    bool tracksOf(Session& session, const ValueList& value, std::vector<Track>& tracks, std::optional<Track>& autoTrack,
                  std::string& autoMin){
        for (const ComponentValue& v : words(value)){
            if (v.isBlock(TokenType::LeftBracket))
                continue;   // line names
            if (v.isFunction("repeat")){
                std::vector<ValueList> args = splitAtCommas(v.children);
                if (args.size() != 2)
                    return false;
                ValueList count = words(args[0]);
                ValueList list = words(args[1]);
                if (count.size() != 1)
                    return false;
                if (isIdent(count[0], "auto-fill") || isIdent(count[0], "auto-fit")){
                    if (list.size() != 1)
                        return false;
                    std::optional<Track> t = trackOf(session, list[0]);
                    if (!t)
                        return false;
                    autoTrack = t;
                    if (list[0].isFunction("minmax")){
                        std::vector<ValueList> mm = commaGroups(list[0].children);
                        if (mm.size() == 2 && mm[0].size() == 1)
                            autoMin = lengthText(mm[0][0]);
                    }else if (t->kind == Track::Kind::Fixed){
                        autoMin = t->length;
                    }
                    continue;
                }
                if (!count[0].isToken(TokenType::Number) || count[0].token.number < 1)
                    return false;
                const int n = std::min(64, (int)count[0].token.number);
                for (int i = 0; i < n; i++){
                    for (const ComponentValue& item : list){
                        if (item.isBlock(TokenType::LeftBracket))
                            continue;
                        std::optional<Track> t = trackOf(session, item);
                        if (!t)
                            return false;
                        tracks.push_back(*t);
                    }
                }
                continue;
            }
            std::optional<Track> t = trackOf(session, v);
            if (!t)
                return false;
            tracks.push_back(*t);
        }
        return true;
    }

    std::string percent(double value){
        return formatNumber(value, 2) + "%";
    }

    // the generated "> *" flex rules of a column template
    void gridColumns(DeclContext& context, const ValueList& value){
        std::vector<Track> tracks;
        std::optional<Track> autoTrack;
        std::string autoMin;
        if (!tracksOf(context.session, value, tracks, autoTrack, autoMin)){
            context.reportAs(Severity::Warning, codes::ForkFallback, "'" + context.sourceText() +
                             "' cannot be approximated with flex until the fork has Grid; ignored", "use flex-wrap with explicit item widths");
            return;
        }
        if (autoTrack){
            const std::string basis = autoMin.empty() ? (autoTrack->kind == Track::Kind::Fixed ? autoTrack->length : "auto") : autoMin;
            const bool grows = autoTrack->kind != Track::Kind::Fixed || !autoMin.empty();
            context.generate(" > *", "flex", grows ? "1 1 " + basis : "0 0 " + basis);
        }else if (!tracks.empty()){
            const size_t k = tracks.size();
            bool uniform = true;
            for (const Track& t : tracks){
                if (t.kind != tracks[0].kind || t.length != tracks[0].length || t.fraction != tracks[0].fraction)
                    uniform = false;
            }
            double totalFraction = 0;
            for (const Track& t : tracks){
                if (t.kind == Track::Kind::Fraction)
                    totalFraction += t.fraction;
            }
            auto flexOf = [&](const Track& t) -> std::string {
                if (t.kind == Track::Kind::Fixed)
                    return "0 0 " + t.length;
                if (t.kind == Track::Kind::Auto)
                    return "1 1 auto";
                // equal bases small enough for k per line and too big for k + 1, then grown by fr
                const double share = totalFraction > 0 ? t.fraction / totalFraction : 1.0 / (double)k;
                return formatNumber(t.fraction) + " 1 " + percent(share * 100.0 * (double)k / (double)(k + 1) + 0.1);
            };
            if (uniform){
                context.generate(" > *", "flex", flexOf(tracks[0]));
            }else{
                for (size_t i = 0; i < k; i++)
                    context.generate(" > :nth-child(" + std::to_string(k) + "n+" + std::to_string(i + 1) + ")", "flex", flexOf(tracks[i]));
            }
        }
        context.reportAs(Severity::Info, codes::ForkFallback, "'" + context.sourceText() +
                         "' is approximated with flex items until the fork has Grid");
    }

}

void jasperengine::htmlui::compat::rewriteDisplay(DeclContext& context){
    ValueList w = words(context.value);
    std::string keyword;
    for (const ComponentValue& v : w){
        if (!v.isToken(TokenType::Ident)){
            if (w.size() == 1 && isVar(v)){
                context.emit("display", text(v));
                return;
            }
            unsupportedValue(context);
            return;
        }
        keyword += (keyword.empty() ? "" : " ") + identOf(v);
    }
    if (keyword == "grid" || keyword == "inline-grid" || keyword == "block grid" || keyword == "inline grid"){
        const bool inlineGrid = keyword.compare(0, 6, "inline") == 0;
        if (context.fork(ForkFeature::Grid)){
            context.emit("display", inlineGrid ? "inline-grid" : "grid");
            return;
        }
        context.emit("display", inlineGrid ? "inline-flex" : "flex");
        context.emit("flex-wrap", "wrap");
        context.reportAs(Severity::Warning, codes::ForkFallback, "'display: " + keyword +
                         "' is drawn as a wrapping flex container until the fork has Grid", "lay the items out with flex");
        return;
    }
    if ((keyword == "-webkit-box" || keyword == "-webkit-inline-box") && ruleDeclares(context, {"-webkit-line-clamp", "line-clamp"})){
        // the line-clamp pattern: a block cut at n lines
        context.emit("display", "block");
        return;
    }
    const ValueSupport* row = findValue("display", keyword);
    if (row && row->support == Support::Native){
        context.emit("display", keyword);
        return;
    }
    if (!row){
        unsupportedValue(context, "use block, inline, inline-block, flex, inline-flex, table or none");
        return;
    }
    const std::string output = row->output;
    if (keyword == "contents"){
        context.reportAs(Severity::Warning, codes::ApproximatedVisible, "'display: contents' is drawn as a block (the element keeps its box)",
                         "remove the wrapper element");
    }else if (row->support == Support::Approximated){
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'display: " + keyword + "' is drawn as " + output);
    }else if (keyword[0] == '-'){
        context.reportAs(Severity::Info, codes::VendorPrefix, "'display: " + keyword + "' is read as " + output);
    }
    context.emit("display", output);
}

void jasperengine::htmlui::compat::rewritePosition(DeclContext& context){
    ValueList w = words(context.value);
    const std::string k = w.size() == 1 ? identOf(w[0]) : std::string();
    if (k == "static" || k == "relative" || k == "absolute"){
        context.emit("position", k);
        return;
    }
    if (k == "fixed"){
        if (!context.fork(ForkFeature::Fixed))
            context.reportAs(Severity::Info, codes::ForkFallback, "'position: fixed' acts like 'absolute' until the fork has Fixed",
                             "place the element in a screen-sized container");
        context.emit("position", "fixed");
        return;
    }
    if (k == "sticky" || k == "-webkit-sticky"){
        context.reportAs(Severity::Warning, codes::ApproximatedVisible, "'position: " + k + "' is drawn as 'relative' (it does not stick)",
                         "pin the element with position: absolute in a non-scrolling container");
        context.emit("position", "relative");
        return;
    }
    if (w.size() == 1 && isVar(w[0])){
        context.emit("position", text(w[0]));
        return;
    }
    unsupportedValue(context, "use static, relative, absolute or fixed");
}

void jasperengine::htmlui::compat::rewriteLogicalBox(DeclContext& context){
    const std::string& n = context.name;
    ValueList w = words(context.value);
    const bool rtl = context.session.rootDirection == "rtl";
    auto two = [&](const std::string& first, const std::string& second){
        if (w.empty() || w.size() > 2){
            unsupportedValue(context);
            return;
        }
        const std::string a = text(w[0]);
        const std::string b = w.size() > 1 ? text(w[1]) : a;
        processSynthetic(context, first, a);
        processSynthetic(context, second, b);
    };
    const std::string left = rtl ? "right" : "left", right = rtl ? "left" : "right";
    if (rtl && (n.find("inline") != std::string::npos))
        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'" + n + "' is mapped right-to-left (the document root is rtl)");

    struct Mapping { const char* logical; std::string physical; };
    const Mapping single[] = {
        {"inline-size", "width"}, {"block-size", "height"}, {"min-inline-size", "min-width"}, {"max-inline-size", "max-width"},
        {"min-block-size", "min-height"}, {"max-block-size", "max-height"},
        {"margin-inline-start", "margin-" + left}, {"margin-inline-end", "margin-" + right},
        {"margin-block-start", "margin-top"}, {"margin-block-end", "margin-bottom"},
        {"padding-inline-start", "padding-" + left}, {"padding-inline-end", "padding-" + right},
        {"padding-block-start", "padding-top"}, {"padding-block-end", "padding-bottom"},
        {"inset-inline-start", left}, {"inset-inline-end", right}, {"inset-block-start", "top"}, {"inset-block-end", "bottom"},
    };
    for (const Mapping& m : single){
        if (n == m.logical){
            processSynthetic(context, m.physical, context.valueText());
            return;
        }
    }
    if (n == "margin-inline")
        two("margin-" + left, "margin-" + right);
    else if (n == "margin-block")
        two("margin-top", "margin-bottom");
    else if (n == "padding-inline")
        two("padding-" + left, "padding-" + right);
    else if (n == "padding-block")
        two("padding-top", "padding-bottom");
    else if (n == "inset-inline")
        two(left, right);
    else if (n == "inset-block")
        two("top", "bottom");
    else
        unsupportedValue(context);
}

void jasperengine::htmlui::compat::rewriteSizing(DeclContext& context){
    const std::string& n = context.name;
    ValueList w = words(context.value);
    if (w.size() == 1){
        std::string keyword = identOf(w[0]);
        if (w[0].isFunction("fit-content"))
            keyword = "fit-content()";
        if (!keyword.empty()){
            if (const ValueSupport* row = findValue(n, keyword)){
                const std::string output = row->output;
                if (row->support == Support::Approximated){
                    const bool info = keyword == "-webkit-fill-available" && output == "100%";
                    if (info)
                        context.reportAs(Severity::Info, codes::ApproximatedMinor, "'" + n + ": " + keyword + "' is written as 100%");
                    else
                        context.reportAs(Severity::Warning, codes::ApproximatedVisible, "'" + n + ": " + keyword + "' is not supported; written as '" +
                                         output + "'", "set an explicit size, or use flex");
                }
                context.emit(n, output);
                return;
            }
        }
        if (w[0].isToken(TokenType::Number) && w[0].token.number == 0){
            context.emit(n, "0px");
            return;
        }
    }
    if (!RcssGrammar::accepts(n, context.valueText())){
        unsupportedValue(context, n.compare(0, 4, "min-") == 0 ? "use a length or percentage" : "use auto, a length or a percentage");
        return;
    }
    context.emit(n, context.valueText());
}

void jasperengine::htmlui::compat::rewriteOverflow(DeclContext& context){
    const std::string& n = context.name;
    ValueList w = words(context.value);
    if (n.compare(0, 19, "overscroll-behavior") == 0){
        std::string k = w.empty() ? std::string() : identOf(w[0]);
        if (k == "none")
            k = "contain";
        if (k != "auto" && k != "contain"){
            unsupportedValue(context, "use auto, contain or none");
            return;
        }
        context.emit("overscroll-behavior", k);
        return;
    }
    std::vector<std::string> values;
    for (const ComponentValue& v : w){
        std::string k = identOf(v);
        if (k == "clip")
            k = "hidden";
        else if (k == "overlay" || k == "-webkit-paged-x" || k == "-webkit-paged-y")
            k = "auto";
        else if (k.empty()){
            if (isVar(v)){
                context.emit(n, context.valueText());
                return;
            }
            unsupportedValue(context, "use visible, hidden, auto or scroll");
            return;
        }
        values.push_back(k);
    }
    if (!RcssGrammar::accepts(n, join(values))){
        unsupportedValue(context, "use visible, hidden, auto or scroll");
        return;
    }
    context.emit(n, join(values));
}

void jasperengine::htmlui::compat::rewriteGrid(DeclContext& context){
    const std::string& n = context.name;
    if (context.fork(ForkFeature::Grid)){
        context.emit(n, context.valueText());
        return;
    }
    if (n == "grid-template-columns"){
        gridColumns(context, context.value);
        return;
    }
    if (n == "grid-template" || n == "grid"){
        // the columns after '/'
        ValueList columns;
        bool afterSlash = false;
        for (const ComponentValue& v : context.value){
            if (v.isToken(TokenType::Delim) && v.token.isDelim('/')){
                afterSlash = true;
                continue;
            }
            if (afterSlash)
                columns.push_back(v);
        }
        if (afterSlash && !columns.empty()){
            ValueList trimmed = trimWhitespace(columns);
            if (!trimmed.empty() && isIdent(trimmed[0], "auto-flow"))
                trimmed.erase(trimmed.begin());
            gridColumns(context, trimmed);
            return;
        }
    }
    context.reportAs(Severity::Info, codes::ForkFallback, "'" + n + "' is ignored until the fork has Grid (the grid is drawn with flex items)");
}
