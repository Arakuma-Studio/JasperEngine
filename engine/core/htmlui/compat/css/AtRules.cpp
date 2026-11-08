// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/AtRules.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/Hash.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssWriter.h"
#include "htmlui/compat/css/RewriteSession.h"
#include "htmlui/compat/css/SelectorRewriter.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/props/Rewriters.h"

#include <algorithm>
#include <cmath>
#include <set>

using namespace jasperengine::htmlui::compat;

namespace {

    bool isWs(const ComponentValue& v){
        return v.isWhitespace() || v.isToken(TokenType::Comment);
    }

    std::vector<const ComponentValue*> significant(const ValueList& values){
        std::vector<const ComponentValue*> out;
        for (const ComponentValue& v : values){
            if (!isWs(v))
                out.push_back(&v);
        }
        return out;
    }

    // ---- @supports

    class Supports {
    public:
        Supports(Session& session, uint32_t file): session(session), file(file) {}

        bool condition(const std::vector<const ComponentValue*>& items, size_t begin, size_t end, bool& valid){
            if (begin >= end){
                valid = false;
                return false;
            }
            if (items[begin]->isToken(TokenType::Ident) && equalsNoCase(items[begin]->token.value, "not")){
                if (end - begin != 2){
                    valid = false;
                    return false;
                }
                return !inParens(*items[begin + 1], valid);
            }
            bool result = inParens(*items[begin], valid);
            std::string joiner;
            for (size_t i = begin + 1; i < end; i += 2){
                if (!items[i]->isToken(TokenType::Ident) || i + 1 >= end){
                    valid = false;
                    return false;
                }
                const std::string word = lowerAscii(items[i]->token.value);
                if ((word != "and" && word != "or") || (!joiner.empty() && joiner != word)){
                    valid = false;
                    return false;
                }
                joiner = word;
                const bool next = inParens(*items[i + 1], valid);
                result = word == "and" ? (result && next) : (result || next);
            }
            return result;
        }

    private:
        bool inParens(const ComponentValue& value, bool& valid){
            if (value.kind == ComponentValue::Kind::Function){
                const std::string name = lowerAscii(value.name());
                if (name == "selector"){
                    std::optional<SelectorList> list = SelectorRewriter::parseList(value.children, file);
                    if (!list)
                        return false;
                    return SelectorRewriter::supported(*list, session.selectorOptions);
                }
                if (name == "font-format"){
                    const std::string format = lowerAscii(serialize(trimWhitespace(value.children)));
                    return format == "truetype" || format == "opentype" || format == "collection";
                }
                if (name == "font-tech")
                    return false;
                return false;   // <general-enclosed>
            }
            if (!value.isBlock(TokenType::LeftParen))
                return false;
            std::vector<const ComponentValue*> inner;
            for (const ComponentValue& v : value.children){
                if (!isWs(v))
                    inner.push_back(&v);
            }
            if (inner.empty())
                return false;
            // ( condition ) or ( declaration )
            if (inner.size() >= 2 && inner[0]->isToken(TokenType::Ident) && inner[1]->isToken(TokenType::Colon))
                return declaration(value);
            if (inner[0]->isBlock(TokenType::LeftParen) || inner[0]->kind == ComponentValue::Kind::Function ||
                (inner[0]->isToken(TokenType::Ident) && equalsNoCase(inner[0]->token.value, "not"))){
                bool innerValid = true;
                const bool result = condition(inner, 0, inner.size(), innerValid);
                return innerValid && result;
            }
            return false;
        }

        // (property: value): supported by the tables and the rewriter keeps the value
        bool declaration(const ComponentValue& block){
            const std::string text = serialize(block.children);
            std::optional<Declaration> declaration = parseDeclaration(file, text, 0, nullptr);
            if (!declaration)
                return false;
            if (declaration->isCustomProperty())
                return true;
            const PropertySupport* row = findProperty(declaration->name);
            if (!row){
                // a vendor-prefixed name of a known property
                for (const char* prefix : {"-webkit-", "-moz-", "-ms-", "-o-"}){
                    const std::string p(prefix);
                    if (declaration->name.compare(0, p.size(), p) == 0)
                        row = findProperty(declaration->name.substr(p.size()));
                }
                if (!row)
                    return false;
            }
            const Support support = effectiveSupport(*row, session.options.fork);
            if (support == Support::Unsupported || support == Support::Ignored)
                return false;
            // the value: the declaration pipeline on a scratch sink must keep it
            SourceFiles scratchFiles;
            DiagnosticSink scratch(scratchFiles);
            DiagnosticSink* real = session.sink;
            RewriteResult* realResult = session.result;
            RewriteResult scratchResult;
            session.sink = &scratch;
            session.result = &scratchResult;
            RuleOutput output;
            const auto important = session.important;
            processDeclarations(session, nullptr, RuleOrigin::Author, false, {*declaration}, output);
            session.important = important;
            session.sink = real;
            session.result = realResult;
            for (const Diagnostic& d : scratch.diagnostics()){
                if (d.code == codes::UnsupportedValue || d.code == codes::UnknownProperty || d.code == codes::UnsupportedProperty ||
                    d.code == codes::UnsupportedUnit || d.code == codes::UnsupportedFunction || d.code == codes::ColorUnresolvable)
                    return false;
            }
            return !output.declarations.empty();
        }

        Session& session;
        uint32_t file;
    };

    // ---- @font-face

    struct FontSource {
        std::string url;
        std::string format;      // lowercase format()/tech() or the extension
        bool local = false;
        SourceRef ref;
    };

    bool usableFormat(const std::string& format){
        return format == "truetype" || format == "opentype" || format == "ttf" || format == "otf" || format == "ttc" ||
               format == "collection" || format == "truetype-variations" || format == "opentype-variations" ||
               // a bitmap font (S51 R15): the engine's own .bitmapfont and AngelCode BMFont
               format == "bitmapfont" || format == "fnt";
    }

    std::string extensionOf(const std::string& url){
        std::string path = url;
        const size_t cut = path.find_first_of("?#");
        if (cut != std::string::npos)
            path = path.substr(0, cut);
        const size_t dot = path.rfind('.');
        const size_t slash = path.rfind('/');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return std::string();
        return lowerAscii(path.substr(dot + 1));
    }

    std::vector<FontSource> fontSources(const ValueList& value, uint32_t file){
        std::vector<FontSource> sources;
        for (const ValueList& group : splitAtCommas(value)){
            FontSource source;
            source.ref = extentOf(file, group);
            for (const ComponentValue& v : group){
                if (isWs(v))
                    continue;
                if (v.isToken(TokenType::Url) || v.isToken(TokenType::String)){
                    source.url = v.token.value;
                }else if (v.isFunction("url") || v.isFunction("src")){
                    for (const ComponentValue& c : v.children){
                        if (c.isToken(TokenType::String))
                            source.url = c.token.value;
                    }
                }else if (v.isFunction("local")){
                    source.local = true;
                }else if (v.isFunction("format") || v.isFunction("tech")){
                    std::string f = lowerAscii(serialize(trimWhitespace(v.children)));
                    f.erase(std::remove(f.begin(), f.end(), '"'), f.end());
                    if (v.isFunction("format") || source.format.empty())
                        source.format = f;
                }
            }
            if (source.format.empty() && !source.url.empty())
                source.format = extensionOf(source.url);
            sources.push_back(source);
        }
        return sources;
    }

    std::string unquotedFamily(const ValueList& value){
        std::string name;
        for (const ComponentValue& v : trimWhitespace(value)){
            if (v.isToken(TokenType::String))
                name += v.token.value;
            else if (v.isToken(TokenType::Ident))
                name += v.token.value;
            else if (v.isWhitespace())
                name += ' ';
        }
        return name;
    }

    void writeFontFace(Session& session, const FlatAtRule& at, RcssWriter& writer){
        const Rule& rule = at.rule;
        std::string family;
        SourceRef familyRef = rule.ref;
        std::vector<FontSource> sources;
        SourceRef srcRef = rule.ref;
        std::string weight = "400";
        SourceRef weightRef = rule.ref;
        std::string style = "normal";
        SourceRef styleRef = rule.ref;
        for (const Declaration& d : rule.declarations){
            const SourceRef ref = d.ref;
            if (d.name == "font-family"){
                family = unquotedFamily(d.value);
                familyRef = ref;
            }else if (d.name == "src"){
                sources = fontSources(d.value, at.file);
                srcRef = ref;
            }else if (d.name == "font-weight"){
                std::vector<const ComponentValue*> words = significant(d.value);
                std::vector<double> numbers;
                for (const ComponentValue* w : words){
                    if (w->isToken(TokenType::Number))
                        numbers.push_back(w->token.number);
                    else if (w->isToken(TokenType::Ident) && equalsNoCase(w->token.value, "normal"))
                        numbers.push_back(400);
                    else if (w->isToken(TokenType::Ident) && equalsNoCase(w->token.value, "bold"))
                        numbers.push_back(700);
                }
                weightRef = ref;
                if (numbers.size() == 1){
                    weight = formatNumber(numbers[0]);
                }else if (numbers.size() == 2){
                    const double lo = std::min(numbers[0], numbers[1]);
                    const double hi = std::max(numbers[0], numbers[1]);
                    const double chosen = (400 >= lo && 400 <= hi) ? 400 : lo;
                    weight = formatNumber(chosen);
                    session.sink->report(codes::FontFaceDescriptor, ref,
                                         "font-weight range " + formatNumber(lo) + " " + formatNumber(hi) + " is registered as " + weight +
                                         " (variable fonts render their default instance)", "import static instances of the weights you use");
                }
            }else if (d.name == "font-style"){
                std::vector<const ComponentValue*> words = significant(d.value);
                styleRef = ref;
                if (!words.empty() && words[0]->isToken(TokenType::Ident)){
                    const std::string s = lowerAscii(words[0]->token.value);
                    style = (s == "italic" || s == "oblique") ? "italic" : "normal";
                }
            }else{
                session.sink->report(codes::FontFaceDescriptor, ref, "@font-face descriptor '" + d.name + "' is dropped", std::string(),
                                     std::string(), d.name);
            }
        }
        if (family.empty()){
            session.sink->reportAs(Severity::Warning, codes::FontSource, rule.ref, "@font-face without font-family is dropped",
                                   "add font-family: \"Name\";");
            return;
        }
        // the first TrueType/OpenType source wins
        const FontSource* chosen = nullptr;
        std::vector<std::string> skipped;
        bool skippedLocal = false;
        for (const FontSource& source : sources){
            if (source.local && source.url.empty()){
                skippedLocal = true;
                continue;
            }
            if (source.url.empty())
                continue;
            if (usableFormat(source.format) && !chosen){
                chosen = &source;
                continue;
            }
            if (!chosen && !source.format.empty())
                skipped.push_back(source.format);
        }
        if (skippedLocal)
            session.sink->reportAs(Severity::Info, codes::FontSource, srcRef, "local() font sources are skipped (fonts come from the project)");
        if (!skipped.empty()){
            std::string list;
            for (const std::string& s : skipped)
                list += (list.empty() ? "" : ", ") + s;
            session.sink->reportAs(Severity::Info, codes::FontSource, srcRef, "font sources in " + list + " format are skipped (the engine reads TTF/OTF and bitmap fonts)");
        }
        if (!chosen){
            // its own subject: the Info about skipped formats has the same code and location
            session.sink->reportAs(Severity::Warning, codes::FontSource, srcRef.valid() ? srcRef : rule.ref,
                                   "@font-face '" + family + "' has no TrueType/OpenType or bitmap font source; the rule is dropped",
                                   "convert the font to TTF (remote fonts are imported as TTF by the editor)", std::string(), family);
            return;
        }
        const std::string path = props::resolveUrlIn(session, chosen->url, at.file, chosen->ref, "font");
        if (path.empty())
            return;
        // LoadFontFace does not go through JoinPath: asset-relative, no leading slash
        std::string assetPath = path;
        while (!assetPath.empty() && assetPath[0] == '/')
            assetPath.erase(0, 1);
        session.result->declaredFontFamilies.push_back(family);
        if (chosen->format == "bitmapfont" || chosen->format == "fnt")
            session.result->bitmapFontFamilies.push_back(family);
        writer.atRule("@font-face {", rule.ref);
        writer.atRule("font-family: " + family + ";", familyRef);
        writer.atRule("src: " + assetPath + ";", srcRef);
        writer.atRule("font-weight: " + weight + ";", weightRef);
        writer.atRule("font-style: " + style + ";", styleRef);
        writer.atRule("}", rule.ref);
    }

    // ---- @keyframes

    struct KeyframeBlock {
        std::vector<double> offsets;   // percentages
        ArenaList<OutDecl> declarations;
        SourceRef ref;
    };

    std::string offsetText(double percent){
        if (percent == 0)
            return "0%";
        if (percent == 100)
            return "100%";
        return formatNumber(percent, 4) + "%";
    }

    void writeKeyframes(Session& session, const FlatAtRule& at, RcssWriter& writer){
        const Rule& rule = at.rule;
        const std::string cssName = AtRules::keyframesName(rule);
        if (cssName.empty()){
            session.sink->report(codes::KeyframesInvalid, rule.preludeRef.valid() ? rule.preludeRef : rule.ref,
                                 "@keyframes without a valid name is dropped", "write @keyframes name { ... }");
            return;
        }
        if (rule.atName != "keyframes")
            session.sink->report(codes::VendorPrefix, rule.preludeRef, "@" + rule.atName + " is read as @keyframes");
        const std::string name = session.keyframeName(cssName);

        std::vector<KeyframeBlock> blocks;
        for (const Rule& frame : rule.rules){
            if (frame.kind != Rule::Kind::Style){
                session.sink->report(codes::KeyframesInvalid, frame.ref, "at-rule inside @keyframes is dropped");
                continue;
            }
            KeyframeBlock block;
            block.ref = frame.ref;
            bool valid = true;
            const ValueList prelude = preludeOf(frame);
            for (const ValueList& selector : splitAtCommas(prelude)){
                std::vector<const ComponentValue*> words = significant(selector);
                if (words.size() != 1){
                    valid = false;
                    break;
                }
                const ComponentValue& w = *words[0];
                if (w.isToken(TokenType::Ident) && equalsNoCase(w.token.value, "from"))
                    block.offsets.push_back(0);
                else if (w.isToken(TokenType::Ident) && equalsNoCase(w.token.value, "to"))
                    block.offsets.push_back(100);
                else if (w.isToken(TokenType::Percentage) && w.token.number >= 0 && w.token.number <= 100)
                    block.offsets.push_back(w.token.number);
                else{
                    valid = false;
                    break;
                }
            }
            if (!valid || block.offsets.empty()){
                session.sink->report(codes::KeyframesInvalid, frame.preludeRef, "invalid keyframe selector '" + serialize(trimWhitespace(prelude)) +
                                     "'; the keyframe is dropped", "use from, to or a percentage between 0% and 100%");
                continue;
            }
            DeclarationList declarations;
            for (const Declaration& d : frame.declarations){
                // (the fork reads a keyframe's own easing with AnimationLonghands: Animation.cpp writes it)
                if ((d.name == "animation-timing-function" || d.name == "-webkit-animation-timing-function") &&
                    !session.options.fork.has(ForkFeature::AnimationLonghands)){
                    session.sink->reportAs(Severity::Warning, codes::AnimationApproximated, d.ref,
                                           "per-keyframe animation-timing-function is not supported; dropped",
                                           "set the easing on the animation itself");
                    continue;
                }
                declarations.push_back(d);
            }
            RuleOutput output;
            processDeclarations(session, nullptr, at.origin, true, declarations, output);
            block.declarations = std::move(output.declarations);
            blocks.push_back(std::move(block));
        }
        if (blocks.empty()){
            session.sink->report(codes::KeyframesInvalid, rule.ref, "@keyframes " + cssName + " has no valid keyframes; dropped");
            return;
        }

        auto writeAll = [&](const std::string& keyframesName, bool reversed){
            writer.atRule("@keyframes " + keyframesName + " {", rule.ref);
            for (const KeyframeBlock& block : blocks){
                std::string selector;
                std::vector<double> offsets = block.offsets;
                if (reversed){
                    for (double& o : offsets)
                        o = 100 - o;
                }
                for (double o : offsets)
                    selector += (selector.empty() ? "" : ", ") + offsetText(o);
                writer.atRule(selector + " {", block.ref);
                std::set<std::string> later;
                std::vector<const OutDecl*> kept;
                for (size_t i = block.declarations.size(); i-- > 0;){
                    if (later.insert(block.declarations[i].name).second)
                        kept.insert(kept.begin(), &block.declarations[i]);
                }
                for (const OutDecl* d : kept)
                    writer.atRule(d->name + ": " + d->value + ";", d->ref);
                writer.atRule("}", block.ref);
            }
            writer.atRule("}", rule.ref);
        };
        writeAll(name, false);
        if (session.reversedKeyframes.count(name))
            writeAll(name + "-rev", true);
    }

    void writeRaw(Session& session, const FlatAtRule& at, RcssWriter& writer){
        (void)session;
        const Rule& rule = at.rule;
        std::string head = "@" + rule.atName;
        const std::string prelude = serialize(trimWhitespace(rule.prelude));
        if (!prelude.empty())
            head += " " + prelude;
        writer.atRule(head + " {", rule.ref);
        for (const Declaration& d : rule.declarations)
            writer.atRule(d.name + ": " + serialize(d.value) + ";", d.ref);
        for (const Rule& child : rule.rules){
            std::string line = serialize(trimWhitespace(preludeOf(child))) + " {";
            for (const Declaration& d : child.declarations)
                line += " " + d.name + ": " + serialize(d.value) + ";";
            writer.atRule(line + " }", child.ref);
        }
        writer.atRule("}", rule.ref);
    }

}

bool AtRules::evaluateCondition(Session& session, const ValueList& condition, uint32_t file){
    std::vector<const ComponentValue*> items = significant(condition);
    Supports supports(session, file);
    bool valid = true;
    const bool result = supports.condition(items, 0, items.size(), valid);
    return valid && result;
}

bool AtRules::evaluateSupports(Session& session, const Rule& rule){
    const uint32_t file = rule.ref.file;
    const bool result = evaluateCondition(session, rule.prelude, file);
    session.sink->report(codes::SupportsEvaluated, rule.preludeRef.valid() ? rule.preludeRef : rule.ref,
                         "@supports " + serialize(trimWhitespace(rule.prelude)) + " is " + (result ? "true" : "false") +
                         (result ? ": its rules apply" : ": its rules are dropped"));
    return result;
}

bool AtRules::reportDropped(Session& session, const Rule& rule){
    const std::string name = "@" + rule.atName;
    const AtRuleSupport* row = findAtRule(name);
    const SourceRef at = rule.preludeRef.valid() ? SourceRef{rule.ref.file, rule.ref.offset, rule.preludeRef.offset + rule.preludeRef.length - rule.ref.offset}
                                                 : rule.ref;
    if (!row){
        session.sink->reportAs(Severity::Warning, codes::UnsupportedAtRule, at, "unknown at-rule '" + name + "' is dropped with its contents",
                               std::string(), std::string(), name);
        return true;
    }
    if (row->support == Support::Unsupported){
        session.sink->reportAs(Severity::Warning, codes::UnsupportedAtRule, at, "'" + name + "' is not supported; dropped with its contents",
                               row->suggestion, std::string(), name);
        return true;
    }
    if (row->support == Support::Ignored){
        session.sink->reportAs(Severity::Info, codes::UnsupportedAtRule, at, "'" + name + "' has no effect in the engine; dropped",
                               row->suggestion, std::string(), name);
        return true;
    }
    return false;
}

std::string AtRules::fontFaceFamily(const Rule& rule){
    for (const Declaration& d : rule.declarations){
        if (d.name == "font-family")
            return unquotedFamily(d.value);
    }
    return std::string();
}

std::string AtRules::keyframesName(const Rule& rule){
    std::vector<const ComponentValue*> words = significant(rule.prelude);
    if (words.size() != 1)
        return std::string();
    if (words[0]->isToken(TokenType::Ident) || words[0]->isToken(TokenType::String))
        return words[0]->token.value;
    return std::string();
}

bool AtRules::validKeyframesName(const std::string& name){
    if (name.empty())
        return false;
    for (char c : name){
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    return true;
}

void AtRules::write(Session& session, const FlatAtRule& rule, RcssWriter& writer){
    const std::string& name = rule.rule.atName;
    if (name == "font-face")
        writeFontFace(session, rule, writer);
    else if (name == "keyframes" || name == "-webkit-keyframes" || name == "-moz-keyframes" || name == "-o-keyframes")
        writeKeyframes(session, rule, writer);
    else
        writeRaw(session, rule, writer);
}
