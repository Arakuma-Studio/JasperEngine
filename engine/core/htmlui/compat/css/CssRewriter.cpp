// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SEAM (phase-2 §4): the pass-through rewriter p2-core ships so the loader, the runtime and
// the UA sheet work before p2-css lands. It parses each sheet, flattens nested rules by
// prefixing their parents, writes declarations re-serialized (double-quoted strings, no
// !important), keeps @media/@keyframes/@font-face/@decorator/@spritesheet blocks, drops
// other at-rules, and collects remote URLs. p2-css replaces this file with §3.9.

#include "htmlui/compat/css/CssRewriter.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RcssWriter.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

struct CssRewriter::Impl {
    CompatOptions options;
    SourceFiles& files;
    DiagnosticSink& sink;
    RewriteContext context;
    RewriteResult* result = nullptr;

    Impl(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, RewriteContext context):
        options(options), files(files), sink(sink), context(std::move(context)) {}

    static std::string trimmed(const std::vector<ComponentValue>& values){
        return serialize(trimWhitespace(values));
    }

    // selectors of a nested rule against its parent's list ("&" replaced, or a descendant)
    static std::vector<std::string> combine(const std::vector<std::string>& parents, const std::vector<ComponentValue>& prelude){
        std::vector<std::string> result;
        for (const std::vector<ComponentValue>& group : splitAtCommas(prelude)){
            const std::string child = trimmed(group);
            if (child.empty())
                continue;
            if (parents.empty()){
                result.push_back(child);
                continue;
            }
            for (const std::string& parent : parents){
                if (child.find('&') != std::string::npos){
                    std::string combined;
                    for (char c : child){
                        if (c == '&')
                            combined += parent;
                        else
                            combined += c;
                    }
                    result.push_back(combined);
                }else{
                    result.push_back(parent + " " + child);
                }
            }
        }
        return result;
    }

    static std::string join(const std::vector<std::string>& selectors){
        std::string text;
        for (size_t i = 0; i < selectors.size(); i++){
            if (i > 0)
                text += ", ";
            text += selectors[i];
        }
        return text;
    }

    void collectUrls(const std::vector<ComponentValue>& values, const std::string& fromFile, const char* kind){
        for (const ComponentValue& value : values){
            std::string url;
            SourceRef ref{0, value.offset, value.length};
            if (value.isToken(TokenType::Url)){
                url = value.token.value;
            }else if (value.isFunction("url") || value.isFunction("src")){
                for (const ComponentValue& child : value.children){
                    if (child.isToken(TokenType::String)){
                        url = child.token.value;
                        break;
                    }
                }
            }else if (value.kind != ComponentValue::Kind::Token){
                collectUrls(value.children, fromFile, kind);
                continue;
            }else if (value.isToken(TokenType::String) && std::string(kind) == "stylesheet"){
                url = value.token.value;   // @import "x.css"
            }
            if (url.empty() || UrlResolver::classify(url) != UrlKind::Remote)
                continue;
            ref.file = files.find(fromFile);
            std::string resolvedKind = kind;
            if (resolvedKind == "stylesheet" && url.find("fonts.googleapis.com") != std::string::npos)
                resolvedKind = "google-fonts-css";
            result->remoteRefs.push_back(RemoteReference{url, resolvedKind, ref});
        }
    }

    void writeDeclarations(RcssWriter& writer, const std::vector<Declaration>& declarations, const std::string& fromFile){
        for (const Declaration& declaration : declarations){
            collectUrls(declaration.value, fromFile, "image");
            if (declaration.name == "font-family")
                result->fontFamilies.push_back(trimmed(declaration.value));
            writer.declaration(declaration.name, serialize(declaration.value), declaration.ref);
        }
    }

    std::string blockText(const Rule& rule, const std::string& fromFile){
        std::string text = "@" + rule.atName;
        const std::string prelude = trimmed(rule.prelude);
        if (!prelude.empty())
            text += " " + prelude;
        text += " {\n";
        for (const Declaration& declaration : rule.declarations){
            collectUrls(declaration.value, fromFile, rule.atName == "font-face" ? "font" : "image");
            text += declaration.name + ": " + serialize(declaration.value) + ";\n";
        }
        for (const Rule& child : rule.rules){
            text += trimmed(child.prelude) + " {";
            for (const Declaration& declaration : child.declarations)
                text += " " + declaration.name + ": " + serialize(declaration.value) + ";";
            text += " }\n";
        }
        text += "}";
        return text;
    }

    void writeRules(RcssWriter& writer, const std::vector<Rule>& rules, const std::vector<std::string>& parents,
                    const std::string& fromFile, bool inMedia){
        for (const Rule& rule : rules){
            if (rule.kind == Rule::Kind::Style){
                const std::vector<std::string> selectors = combine(parents, rule.prelude);
                if (selectors.empty())
                    continue;
                if (!rule.declarations.empty()){
                    writer.beginRule(join(selectors), rule.ref);
                    writeDeclarations(writer, rule.declarations, fromFile);
                    writer.endRule();
                }
                writeRules(writer, rule.rules, selectors, fromFile, inMedia);
                continue;
            }

            if (rule.atName == "media"){
                if (!inMedia){
                    RcssMediaCondition condition;
                    condition.raw = trimmed(rule.prelude);
                    writer.beginMedia(condition, rule.ref);
                }
                if (!rule.declarations.empty() && !parents.empty()){
                    writer.beginRule(join(parents), rule.ref);
                    writeDeclarations(writer, rule.declarations, fromFile);
                    writer.endRule();
                }
                writeRules(writer, rule.rules, parents, fromFile, true);
                if (!inMedia)
                    writer.endMedia();
                continue;
            }

            if (rule.atName == "import"){
                collectUrls(rule.prelude, fromFile, "stylesheet");
                continue;
            }

            if (rule.atName == "font-face"){
                for (const Declaration& declaration : rule.declarations){
                    if (declaration.name == "font-family")
                        result->declaredFontFamilies.push_back(trimmed(declaration.value));
                }
                writer.atRule(blockText(rule, fromFile), rule.ref);
                continue;
            }

            if (rule.atName == "keyframes" || rule.atName == "decorator" || rule.atName == "spritesheet"){
                writer.atRule(blockText(rule, fromFile), rule.ref);
                continue;
            }
            // other at-rules: dropped by the pass-through
        }
    }

    StylesheetOutput rewriteOne(const StylesheetInput& input){
        StylesheetOutput output;
        output.kind = input.kind;
        output.elementLoc = input.elementLoc;
        output.sourcePath = files.path(input.file);

        if (input.kind == StylesheetInput::Kind::StyleAttribute){
            RcssWriter writer(RcssWriter::Mode::DeclarationList);
            const std::vector<Declaration> declarations = parseDeclarationList(input.file, input.text, input.baseOffset, sink);
            writeDeclarations(writer, declarations, output.sourcePath);
            output.rcss = writer.take(output.lines);
            return output;
        }

        RcssWriter writer;
        const Stylesheet sheet = parseStylesheet(input.file, input.text, sink, input.baseOffset);
        if (!input.media.empty()){
            RcssMediaCondition condition;
            condition.raw = input.media;
            writer.beginMedia(condition);
            writeRules(writer, sheet.rules, {}, output.sourcePath, true);
            writer.endMedia();
        }else{
            writeRules(writer, sheet.rules, {}, output.sourcePath, false);
        }
        output.rcss = writer.take(output.lines);

        if (input.kind == StylesheetInput::Kind::File){
            if (!context.virtualDirectory.empty())
                output.virtualPath = context.virtualDirectory + output.sourcePath + ".rcss";
            if (!output.sourcePath.empty())
                result->dependencies.push_back(output.sourcePath);
        }
        return output;
    }
};

CssRewriter::CssRewriter(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, RewriteContext context):
    impl(std::make_unique<Impl>(options, files, sink, std::move(context))) {
}

CssRewriter::~CssRewriter() = default;

RewriteResult CssRewriter::rewrite(Span<const StylesheetInput> inputs){
    RewriteResult result;
    result.importantSheet.kind = StylesheetInput::Kind::Inline;
    impl->result = &result;
    for (const StylesheetInput& input : inputs)
        result.sheets.push_back(impl->rewriteOne(input));
    impl->result = nullptr;

    std::sort(result.dependencies.begin(), result.dependencies.end());
    result.dependencies.erase(std::unique(result.dependencies.begin(), result.dependencies.end()), result.dependencies.end());
    return result;
}

std::string CssRewriter::rewriteSelector(std::string_view selector, DiagnosticSink*, const CompatOptions&){
    const std::vector<ComponentValue> values = parseComponentValueList(selector);
    return serialize(trimWhitespace(values));
}

std::string CssRewriter::rewriteDeclarations(std::string_view css, DiagnosticSink* sink, const CompatOptions&){
    SourceFiles files;
    const uint32_t file = files.add("[style]", std::string(css));
    DiagnosticSink localSink(files);
    const std::vector<Declaration> declarations = parseDeclarationList(file, css, 0, localSink);
    if (sink){
        for (const Diagnostic& diagnostic : localSink.diagnostics())
            sink->report(diagnostic);
    }
    RcssWriter writer(RcssWriter::Mode::DeclarationList);
    for (const Declaration& declaration : declarations)
        writer.declaration(declaration.name, serialize(declaration.value), declaration.ref);
    return writer.take();
}
