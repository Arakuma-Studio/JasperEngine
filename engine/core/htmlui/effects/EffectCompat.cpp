// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// effect() and @effect in the compatibility layer (phase-5 spec 3.5, 3.13).

#include "EffectCompat.h"
#include "EffectRegistry.h"
#include "EffectSyntax.h"

#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/Units.h"
#include "htmlui/compat/css/props/Rewriters.h"

#include <algorithm>
#include <map>
#include <mutex>

using namespace jasperengine;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    const char* const kCatalogue = "see jasper://docs/html-ui-effects";

    // the function as written, and where its bytes are in the file (diagnostics point into it)
    struct FunctionSource {
        std::string text;
        uint32_t file = 0;
        uint32_t base = 0;
        bool exact = false;
    };

    FunctionSource sourceOf(hc::Session& session, const hc::ComponentValue& function, uint32_t file){
        FunctionSource source;
        if (function.length > 0 && file != 0){
            const std::string_view slice = session.files.slice(hc::SourceRef{file, function.offset, function.length});
            if (slice.size() >= 7 && hc::equalsNoCase(slice.substr(0, 7), "effect(")){
                source.text = std::string(slice);
                source.file = file;
                source.base = function.offset;
                source.exact = true;
                return source;
            }
        }
        source.text = hc::serialize(function);
        return source;
    }

    void report(hc::DiagnosticSink& sink, const std::vector<EffectDiagnostic>& diags, const FunctionSource& source, hc::SourceRef fallback){
        for (const EffectDiagnostic& d : diags){
            if (!d.file.empty()){
                // located by its reporter (a project effect's manifest: the .frag and its line)
                sink.report(d);
                continue;
            }
            EffectDiagnostic located = d;
            located.line = 0;
            located.column = 0;
            located.length = 0;
            hc::SourceRef ref = fallback;
            if (source.exact && d.column > 0)
                ref = hc::SourceRef{source.file, source.base + d.column - 1, std::max<uint32_t>(d.length, 1)};
            sink.report(located, ref);
        }
    }

    hc::SourceRef refOf(const hc::SourceRef& ref, const FunctionSource& source){
        return source.exact ? hc::SourceRef{source.file, source.base, (uint32_t)source.text.size()} : ref;
    }

    std::string join(const std::vector<std::string>& items, size_t limit = 5){
        std::string text;
        for (size_t i = 0; i < items.size() && i < limit; i++)
            text += (text.empty() ? "" : ", ") + items[i];
        return text;
    }

    // the effect of a call, or nullptr with effect-unknown reported
    const EffectManifest* findEffect(const EffectCall& call, hc::DiagnosticSink& sink, const FunctionSource& source, hc::SourceRef ref){
        const EffectManifest* m = EffectRegistry::get().find(call.name);
        if (m)
            return m;
        const std::vector<std::string> suggestions = EffectRegistry::get().suggestions(call.name);
        std::string suggestion = suggestions.empty() ? std::string(kCatalogue) : join(suggestions) + "; " + kCatalogue;
        EffectDiagnostic d = makeEffectDiagnostic(hc::codes::EffectUnknown, "unknown effect '" + call.name + "'; dropped", suggestion,
                                                  call.nameOffset, call.name.size());
        report(sink, {d}, source, ref);
        return nullptr;
    }

    // more than four high-cost effects in one compile: one note
    void countCost(hc::Session& session, const EffectManifest& m, hc::DiagnosticSink& sink, hc::SourceRef ref){
        if (m.cost != EffectCost::High)
            return;
        static std::mutex mutex;
        static std::map<const hc::Session*, int> counts;
        std::lock_guard<std::mutex> lock(mutex);
        // (a new session at the same address starts over: entries of finished sessions are kept small)
        if (counts.size() > 64)
            counts.clear();
        const int count = ++counts[&session];
        if (count == 5){
            EffectDiagnostic d = makeEffectDiagnostic(hc::codes::EffectCost, "more than four high-cost effects in this document",
                                                      "keep high-cost effects to a couple of elements or a quarter of the screen "
                                                      "(guide: Effects, performance)");
            sink.report(d, ref);
        }
    }

    std::string blendArgument(const std::string& mode, bool& unsupported){
        unsupported = false;
        const std::string m = hc::lowerAscii(mode);
        if (m.empty() || m == "normal")
            return std::string();
        if (m == "multiply" || m == "screen")
            return m;
        if (m == "plus-lighter" || m == "lighter")
            return "add";
        unsupported = true;
        return std::string();
    }

}

void jasperengine::htmlui::registerEffectCompatHandlers(){
    static std::once_flag once;
    std::call_once(once, [](){
        hc::registerImageFunctionHandler("effect", [](const hc::ImageLayerContext& context, hc::DiagnosticSink& sink){
            return EffectCompat::imageLayer(context, sink);
        });
        hc::registerFilterFunctionHandler("effect", [](const hc::FilterContext& context, hc::DiagnosticSink& sink){
            return EffectCompat::filterFunction(context, sink);
        });
        hc::registerAtRuleHandler("effect", [](const hc::Rule& rule, hc::Session& session, hc::DiagnosticSink& sink){
            EffectCompat::declareEffect(rule, session, sink);
        });
    });
}

std::optional<std::string> EffectCompat::imageLayer(const hc::ImageLayerContext& context, hc::DiagnosticSink& sink){
    hc::Session& session = *context.session;
    const FunctionSource source = sourceOf(session, *context.function, context.file);
    const hc::SourceRef ref = refOf(context.ref, source);
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    if (!EffectSyntax::parse(source.text, call, diags)){
        report(sink, diags, source, ref);
        return std::nullopt;
    }
    const EffectManifest* m = findEffect(call, sink, source, ref);
    if (!m){
        report(sink, diags, source, ref);
        return std::nullopt;
    }
    if (m->kind == EffectKind::Filter){
        const std::string counterpart = EffectRegistry::get().counterpart(*m);
        std::string suggestion = "a filter effect goes in filter or backdrop-filter";
        if (!counterpart.empty())
            suggestion += "; the paint effect '" + counterpart + "' goes in " + context.property;
        diags.push_back(makeEffectDiagnostic(hc::codes::EffectWrongContext,
                                             "'" + m->name + "' is a filter effect: it cannot be an image in " + context.property + "; dropped",
                                             suggestion, call.nameOffset, call.name.size()));
        report(sink, diags, source, ref);
        return std::nullopt;
    }
    // background-size, -position and -repeat do not apply to an effect layer
    const char* prefix = (context.property.rfind("mask", 0) == 0) ? "mask-" : "background-";
    auto ignored = [&](bool set, const char* what){
        if (!set)
            return;
        diags.push_back(makeEffectDiagnostic(hc::codes::EffectIgnoredBackgroundProp,
                                             std::string(prefix) + what + " does not apply to the effect layer '" + m->name + "'; ignored",
                                             "use the effect's own parameters", call.nameOffset, call.name.size()));
    };
    ignored(context.sizeSet, "size");
    ignored(context.positionSet, "position");
    ignored(context.repeatSet, "repeat");
    // background-blend-mode is the layer's blend (a blend argument of the call wins)
    bool unsupported = false;
    const std::string blend = blendArgument(context.blendMode, unsupported);
    if (unsupported)
        diags.push_back(makeEffectDiagnostic(hc::codes::EffectUnsupportedBlend,
                                             "background-blend-mode '" + context.blendMode + "' is not supported for the effect layer '" + m->name +
                                             "'; drawn as normal", "normal, multiply, screen or plus-lighter", call.nameOffset, call.name.size()));
    const bool hasBlend = std::any_of(call.args.begin(), call.args.end(), [](const EffectArg& arg){ return arg.name == "blend"; });
    if (!blend.empty() && !hasBlend && !context.textFill){
        EffectArg arg;
        arg.name = "blend";
        arg.value = blend;
        arg.nameOffset = call.nameOffset;
        arg.valueOffset = call.nameOffset;
        call.args.push_back(arg);
    }
    const std::string area = (context.native || context.textFill) ? std::string() : (context.clip.empty() ? std::string("border-box") : context.clip);
    std::string rcss = EffectSyntax::toRcss(call, *m, context.textFill, area, diags);
    report(sink, diags, source, ref);
    countCost(session, *m, sink, ref);
    return rcss;
}

std::optional<std::string> EffectCompat::filterFunction(const hc::FilterContext& context, hc::DiagnosticSink& sink){
    hc::Session& session = *context.session;
    const FunctionSource source = sourceOf(session, *context.function, context.file);
    const hc::SourceRef ref = refOf(context.ref, source);
    EffectCall call;
    std::vector<EffectDiagnostic> diags;
    if (!EffectSyntax::parse(source.text, call, diags)){
        report(sink, diags, source, ref);
        return std::nullopt;
    }
    const EffectManifest* m = findEffect(call, sink, source, ref);
    if (!m){
        report(sink, diags, source, ref);
        return std::nullopt;
    }
    std::string rcss = EffectSyntax::toRcss(call, *m, true, std::string_view(), diags);
    report(sink, diags, source, ref);
    countCost(session, *m, sink, ref);
    return rcss;
}

void EffectCompat::declareEffect(const hc::Rule& rule, hc::Session& session, hc::DiagnosticSink& sink){
    const hc::SourceRef at = rule.preludeRef.valid() ? rule.preludeRef : rule.ref;
    const std::string name = hc::serialize(hc::trimWhitespace(rule.prelude));
    auto error = [&](hc::DiagCode code, const std::string& message, const std::string& suggestion){
        sink.report(code, at, message, suggestion);
    };
    bool validName = !name.empty() && name[0] >= 'a' && name[0] <= 'z';
    for (char c : name)
        validName = validName && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-');
    if (!validName){
        error(hc::codes::EffectDeclareConflict, "@effect '" + name + "' is not an effect name; dropped",
              "lower case letters, digits and '-', starting with a letter");
        return;
    }
    const hc::Declaration* src = nullptr;
    for (const hc::Declaration& d : rule.declarations){
        if (d.name == "src")
            src = &d;
    }
    std::optional<std::string> url;
    if (src){
        for (const hc::ComponentValue& v : src->value){
            if (!v.isToken(hc::TokenType::Whitespace) && (url = hc::props::urlOf(v)))
                break;
        }
    }
    if (!url){
        error(hc::codes::EffectFileMissing, "@effect " + name + " has no src: url(...); dropped", "@effect " + name + " { src: url(\"fx/" + name + ".frag\"); }");
        return;
    }
    const uint32_t file = rule.ref.file;
    const hc::SourceRef urlRef = src->valueRef.valid() ? src->valueRef : at;
    const hc::ResolvedUrl resolved = session.urls.resolve(*url, session.files.path(file), "effect", urlRef, nullptr);
    if (resolved.assetPath.empty() || !resolved.local()){
        sink.report(hc::codes::EffectFileMissing, urlRef, "@effect " + name + ": src '" + *url + "' is not a project file; dropped",
                    "src resolves against the stylesheet, like url()");
        return;
    }
    // the .frag is a dependency of the document: an edit reloads it (hot reload)
    if (session.result)
        session.result->dependencies.push_back(resolved.assetPath);
    std::vector<EffectDiagnostic> diags;
    const hc::FileAccess& files = session.context.files;
    EffectRegistry::FileReader reader;
    if (files.read)
        reader = [&files](const std::string& path, std::string& content){ return hc::readAssetFile(files, path, content); };
    const uint32_t line = session.files.line(at.file, at.offset);
    if (EffectRegistry::get().declareProjectEffect(name, resolved.assetPath, session.files.path(file), (int)line, diags, reader) && session.result){
        // its includes too: editing one hot-reloads the effect (p5-tooling)
        for (const std::string& include : EffectRegistry::get().includeFilesOf(name))
            session.result->dependencies.push_back(include);
    }
    for (EffectDiagnostic& d : diags){
        if (d.code == hc::codes::EffectFileMissing){
            d.message = "@effect " + name + ": '" + *url + "' not found (no file at " + resolved.assetPath + "); dropped";
            d.line = 0;
            d.column = 0;
            sink.report(d, urlRef);
        }else if (!d.file.empty()){
            sink.report(d);
        }else{
            d.line = 0;
            d.column = 0;
            sink.report(d, at);
        }
    }
}

std::string EffectCompat::paintSuggestion(const std::string& paintName){
    const EffectManifest* m = EffectRegistry::get().find(hc::lowerAscii(paintName));
    return m ? "write effect(" + m->name + ")" : std::string();
}

bool jasperengine::htmlui::compat::rejectsHandledFunction(Session& session, const Declaration& declaration, const std::string& property){
    if (declaration.isCustomProperty())
        return false;
    std::string_view plain = property;
    for (std::string_view prefix : {std::string_view("-webkit-"), std::string_view("-moz-")}){
        if (plain.substr(0, prefix.size()) == prefix)
            plain = plain.substr(prefix.size());
    }
    static const char* const allowed[] = {"background", "background-image", "mask", "mask-image", "filter", "backdrop-filter"};
    for (const char* name : allowed){
        if (plain == name)
            return false;
    }
    for (const ComponentValue& v : declaration.value){
        if (v.kind != ComponentValue::Kind::Function || !equalsNoCase(v.name(), "effect"))
            continue;
        std::string kind;
        EffectCall call;
        std::vector<EffectDiagnostic> diags;
        const FunctionSource source = sourceOf(session, v, declaration.ref.file);
        if (EffectSyntax::parse(source.text, call, diags)){
            if (const EffectManifest* m = EffectRegistry::get().find(call.name))
                kind = m->kind == EffectKind::Paint ? " (a paint effect: background, background-image, mask-image, filter or backdrop-filter)"
                                                    : " (a filter effect: filter or backdrop-filter)";
        }
        const SourceRef ref = source.exact ? SourceRef{source.file, source.base, (uint32_t)source.text.size()} : declaration.ref;
        session.sink->report(codes::EffectWrongContext, ref, "effect() cannot be used in '" + property + "'; declaration dropped",
                             "effect() goes in background, background-image, mask-image, filter or backdrop-filter" + kind);
        return true;
    }
    return false;
}
