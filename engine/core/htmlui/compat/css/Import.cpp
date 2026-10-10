// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/Import.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"
#include "htmlui/compat/css/RewriteSession.h"
#include "htmlui/compat/css/Units.h"

#include <algorithm>

using namespace jasperengine::htmlui::compat;

std::optional<ImportedSheet> Import::load(Session& session, const Rule& rule, uint32_t fromFile, const std::vector<uint32_t>& chain){
    ImportedSheet imported;
    const SourceRef at = rule.ref;
    ValueList prelude = trimWhitespace(rule.prelude);
    if (prelude.empty()){
        session.sink->reportAs(Severity::Warning, codes::ImportProblem, at, "@import without a URL is ignored", "write @import url(\"file.css\");");
        return std::nullopt;
    }
    // the URL: url(x), url("x") or "x"
    std::string url;
    const ComponentValue& first = prelude.front();
    if (first.isToken(TokenType::Url) || first.isToken(TokenType::String)){
        url = first.token.value;
    }else if (first.isFunction("url") || first.isFunction("src")){
        for (const ComponentValue& child : first.children){
            if (child.isToken(TokenType::String))
                url = child.token.value;
        }
    }
    if (url.empty()){
        session.sink->reportAs(Severity::Warning, codes::ImportProblem, at, "@import needs a URL; the rule is ignored",
                              "write @import url(\"file.css\");");
        return std::nullopt;
    }

    // conditions: layer, supports(), media list
    ValueList media;
    for (size_t i = 1; i < prelude.size(); i++){
        const ComponentValue& v = prelude[i];
        if (v.isWhitespace() || v.isToken(TokenType::Comment)){
            if (!media.empty())
                media.push_back(v);
            continue;
        }
        if (media.empty() && v.isToken(TokenType::Ident) && equalsNoCase(v.token.value, "layer")){
            imported.layered = true;
            continue;
        }
        if (media.empty() && v.isFunction("layer")){
            imported.layered = true;
            imported.layer = serialize(trimWhitespace(v.children));
            continue;
        }
        if (media.empty() && v.isFunction("supports")){
            imported.supports = v.children;
            continue;
        }
        media.push_back(v);
    }
    if (!media.empty())
        imported.media = MediaQuery::normalize(trimWhitespace(media), extentOf(fromFile, media), session.options, session.sink);

    const std::string fromPath = session.files.path(fromFile);
    const ResolvedUrl resolved = session.urls.resolve(url, fromPath, "stylesheet", SourceRef{fromFile, first.offset, first.length}, session.sink);
    if (resolved.kind == UrlKind::Remote){
        RemoteReference reference;
        reference.url = url;
        reference.kind = url.find("fonts.googleapis.com") != std::string::npos ? "google-fonts-css" : "stylesheet";
        reference.ref = SourceRef{fromFile, first.offset, first.length};
        session.result->remoteRefs.push_back(reference);
        if (resolved.entry)
            session.result->dependencies.push_back(ImportManifest::kPath);
    }
    if (resolved.dropped() || resolved.assetPath.empty())
        return std::nullopt;
    if (resolved.kind == UrlKind::Data){
        session.sink->reportAs(Severity::Warning, codes::ImportProblem, at, "@import of a data: URI is not supported; the rule is ignored",
                              "put the CSS in a file");
        return std::nullopt;
    }
    const std::string path = resolved.assetPath;

    // cycle and depth
    const uint32_t existing = session.files.find(path);
    if (existing && (existing == fromFile || std::find(chain.begin(), chain.end(), existing) != chain.end())){
        session.sink->reportAs(Severity::Error, codes::ImportProblem, at, "@import cycle: '" + path + "' imports itself; the repeated import is dropped",
                              "remove the circular @import");
        return std::nullopt;
    }
    if ((int)chain.size() >= kMaxDepth){
        session.sink->reportAs(Severity::Warning, codes::ImportProblem, at, "@import nested more than 16 levels deep; '" + path + "' is not read",
                              "flatten the import chain");
        return std::nullopt;
    }

    std::string text;
    if (!readAssetFile(session.context.files, path, text)){
        // UrlResolver reported a missing local file already
        return std::nullopt;
    }
    session.result->dependencies.push_back(path);
    stripUtf8Bom(text);
    const std::vector<uint32_t> invalid = sanitizeUtf8(text);
    const uint32_t file = session.files.add(path, text, SourceRef{fromFile, rule.ref.offset, rule.ref.length});
    if (!invalid.empty()){
        session.sink->report(codes::InvalidUtf8, SourceRef{file, invalid[0], 3},
                            std::to_string(invalid.size()) + " invalid UTF-8 sequence(s) replaced by U+FFFD", "save the file as UTF-8");
    }
    imported.file = file;
    imported.sheet = parseStylesheet(file, session.files.text(file), *session.sink);
    return imported;
}
