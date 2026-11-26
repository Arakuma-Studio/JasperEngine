// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "StylesheetImporter.h"

#include "ContentSniffer.h"
#include "GoogleFontsImporter.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/Hash.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/css/CssParser.h"
#include "htmlui/compat/css/CssSerialize.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using namespace jasperengine::htmlui::compat;

namespace {

    // (ComponentValue::isFunction and Token::isIdent are not exported from the shared engine)
    bool isFunctionNamed(const ComponentValue& value, const char* lowercaseName){
        return value.kind == ComponentValue::Kind::Function && lowerAscii(value.token.value) == lowercaseName;
    }

    bool isIdentNamed(const ComponentValue& value, const char* lowercaseName){
        return value.isToken(TokenType::Ident) && lowerAscii(value.token.value) == lowercaseName;
    }

    // the URL of a url token, url("…") / src("…") function, or (as @import and image-set()
    // write them) a bare string
    bool urlOf(const ComponentValue& value, bool stringIsUrl, std::string& url){
        if (value.isToken(TokenType::Url)){
            url = value.token.value;
            return true;
        }
        if (isFunctionNamed(value, "url") || isFunctionNamed(value, "src")){
            for (const ComponentValue& child : value.children){
                if (child.isToken(TokenType::String)){
                    url = child.token.value;
                    return true;
                }
            }
            return false;
        }
        if (stringIsUrl && value.isToken(TokenType::String)){
            url = value.token.value;
            return true;
        }
        return false;
    }

    void collectValues(const ValueList& values, CssUrl::Context context, int fontFace, bool stringIsUrl,
                       std::vector<CssUrl>& out){
        for (const ComponentValue& value : values){
            std::string url;
            if (urlOf(value, stringIsUrl, url)){
                CssUrl found;
                found.url = url;
                found.offset = value.offset;
                found.length = value.length;
                found.context = context;
                found.fontFace = fontFace;
                out.push_back(std::move(found));
                continue;
            }
            if (value.kind != ComponentValue::Kind::Token){
                const bool imageSet = isFunctionNamed(value, "image-set") || isFunctionNamed(value, "-webkit-image-set");
                collectValues(value.children, context, fontFace, imageSet, out);
            }
        }
    }

    void collectRules(const RuleList& rules, int& fontFaces, std::vector<CssUrl>& out){
        for (const Rule& rule : rules){
            if (rule.isAt("import")){
                for (const ComponentValue& value : rule.prelude){
                    std::string url;
                    if (urlOf(value, true, url)){
                        CssUrl found;
                        found.url = url;
                        found.offset = value.offset;
                        found.length = value.length;
                        found.context = CssUrl::Context::Import;
                        out.push_back(std::move(found));
                        break;
                    }
                }
                continue;
            }
            const bool fontFace = rule.isAt("font-face");
            const int index = fontFace ? fontFaces++ : -1;
            for (const Declaration& declaration : rule.declarations)
                collectValues(declaration.value, fontFace ? CssUrl::Context::FontFace : CssUrl::Context::Other, index, false, out);
            collectRules(rule.rules, fontFaces, out);
        }
    }

    void collectFontFaces(const RuleList& rules, const std::string& css, std::vector<CssFontFace>& out){
        for (const Rule& rule : rules){
            if (!rule.isAt("font-face")){
                collectFontFaces(rule.rules, css, out);
                continue;
            }
            CssFontFace face;
            for (const Declaration& declaration : rule.declarations){
                if (declaration.name == "font-family"){
                    std::string family;
                    for (const ComponentValue& value : declaration.value){
                        if (value.isToken(TokenType::String)){
                            family = value.token.value;
                            break;
                        }
                        if (value.isToken(TokenType::Ident)){
                            if (!family.empty())
                                family += ' ';
                            family += value.token.value;
                        }
                    }
                    face.family = family;
                }else if (declaration.name == "font-weight"){
                    for (const ComponentValue& value : declaration.value){
                        if (value.isToken(TokenType::Number)){
                            face.weight = (int)value.token.number;
                            break;
                        }
                        if (value.isToken(TokenType::Ident)){
                            if (isIdentNamed(value, "bold"))
                                face.weight = 700;
                            break;
                        }
                    }
                }else if (declaration.name == "font-style"){
                    for (const ComponentValue& value : declaration.value){
                        if (value.isToken(TokenType::Ident)){
                            face.italic = isIdentNamed(value, "italic") || isIdentNamed(value, "oblique");
                            break;
                        }
                    }
                }else if (declaration.name == "src"){
                    face.hasSrc = true;
                    face.srcOffset = declaration.valueRef.offset;
                    face.srcLength = declaration.valueRef.length;
                    for (const ValueList& group : splitAtCommas(declaration.value)){
                        const ValueList trimmed = trimWhitespace(group);
                        if (trimmed.empty())
                            continue;
                        CssFontSource source;
                        const SourceRef extent = extentOf(0, trimmed);
                        source.text = css.substr(extent.offset, extent.length);
                        for (const ComponentValue& value : trimmed){
                            std::string url;
                            if (urlOf(value, false, url)){
                                source.url = url;
                            }else if (isFunctionNamed(value, "format")){
                                for (const ComponentValue& child : value.children){
                                    if (child.isToken(TokenType::String) || child.isToken(TokenType::Ident)){
                                        source.format = lowerAscii(child.token.value);
                                        break;
                                    }
                                }
                            }
                        }
                        face.sources.push_back(std::move(source));
                    }
                }
            }
            out.push_back(std::move(face));
        }
    }

    Stylesheet parse(const std::string& css, SourceFiles& files){
        const uint32_t id = files.add("remote.css", css);
        DiagnosticSink sink(files);
        return parseStylesheet(id, files.text(id), sink);
    }

    bool skippedUrl(const std::string& url){
        const UrlKind kind = UrlResolver::classify(url);
        return kind == UrlKind::Empty || kind == UrlKind::Data || kind == UrlKind::Fragment || kind == UrlKind::OtherScheme ||
               kind == UrlKind::Virtual || kind == UrlKind::EnginePath;
    }

}

bool CssFontSource::usable() const{
    if (url.empty())
        return false;
    if (!format.empty())
        return format == "truetype" || format == "opentype" || format == "truetype-variations" || format == "opentype-variations" ||
               format == "collection" || woff();
    const std::string extension = urlExtension(url);
    return extension == "ttf" || extension == "otf" || extension == "ttc" || woff();
}

bool CssFontSource::woff() const{
    if (url.empty())
        return false;
    if (!format.empty())
        return format == "woff" || format == "woff2" || format == "woff2-variations" || format == "woff-variations";
    const std::string extension = urlExtension(url);
    return extension == "woff" || extension == "woff2";
}

std::vector<CssUrl> jasperengine::editor::htmlui::findCssUrls(const std::string& css){
    // the parsed sheet lives in this arena
    Arena arena;
    ArenaScope scope(arena);
    SourceFiles files;
    const Stylesheet sheet = parse(css, files);
    std::vector<CssUrl> urls;
    int fontFaces = 0;
    collectRules(sheet.rules, fontFaces, urls);
    return urls;
}

std::vector<CssFontFace> jasperengine::editor::htmlui::findFontFaces(const std::string& css){
    // the parsed sheet lives in this arena
    Arena arena;
    ArenaScope scope(arena);
    SourceFiles files;
    const Stylesheet sheet = parse(css, files);
    std::vector<CssFontFace> faces;
    collectFontFaces(sheet.rules, css, faces);
    return faces;
}

std::string jasperengine::editor::htmlui::applyCssEdits(const std::string& text, std::vector<CssEdit> edits){
    std::sort(edits.begin(), edits.end(), [](const CssEdit& a, const CssEdit& b){ return a.offset > b.offset; });
    std::string out = text;
    for (const CssEdit& edit : edits){
        if (edit.offset > out.size())
            continue;
        out.replace(edit.offset, std::min<size_t>(edit.length, out.size() - edit.offset), edit.text);
    }
    return out;
}

std::string jasperengine::editor::htmlui::cssUrl(const std::string& path){
    return "url(" + serializeString(path) + ")";
}

// ---------------------------------------------------------------------------------------------
// StylesheetImporter
// ---------------------------------------------------------------------------------------------

StylesheetImporter::StylesheetImporter(RemoteImporter& importer): importer(importer) {}

std::string StylesheetImporter::localPath(const RemoteImporter& importer, const std::string& url, const std::string& extension){
    std::string host, path, query;
    if (!splitUrl(url, host, path, query))
        return "";
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= path.size()){
        size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        const std::string segment = safeFileName(path.substr(start, end - start));
        if (!segment.empty())
            segments.push_back(segment);
        start = end + 1;
    }
    if (segments.empty() || path.back() == '/')
        segments.push_back("index");

    std::string& name = segments.back();
    std::string stem = name;
    std::string currentExtension;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0){
        stem = name.substr(0, dot);
        currentExtension = name.substr(dot + 1);
    }
    std::string finalExtension = currentExtension;
    if (!extension.empty() && lowerAscii(currentExtension) != lowerAscii(extension)){
        // a ".min" or version suffix is part of the stem
        stem = name;
        finalExtension = extension;
    }
    if (!query.empty())
        stem += "-" + compat::sha1Hex(query).substr(0, 8);
    name = finalExtension.empty() ? stem : stem + "." + finalExtension;

    std::string relative = "css/" + safeFileName(host);
    for (const std::string& segment : segments)
        relative += "/" + segment;
    return importer.importPath(relative);
}

ImportedResource StylesheetImporter::run(const std::string& url, const std::string& finalUrl, const std::string& body,
                                         const ImportLocation& at){
    ImportedResource result;
    result.url = url;

    const std::string local = localPath(importer, url, "css");
    done[ImportManifest::normalizeUrl(url)] = local;
    rewriteSheet(finalUrl, body, local, 0, at);

    std::string host, path, query;
    splitUrl(url, host, path, query);
    ImportEntry entry;
    entry.url = url;
    entry.kind = "stylesheet";
    entry.path = local;
    for (const auto& pair : staging.files())
        entry.files.push_back(pair.first);
    for (const std::string& file : nestedFiles){
        if (std::find(entry.files.begin(), entry.files.end(), file) == entry.files.end())
            entry.files.push_back(file);
    }
    entry.licenseName = "unknown";
    entry.source = host;

    if (!importer.finish(staging, {entry}, result))
        return result;
    importer.report(codes::LicenseNote, at, url,
                    "the license of '" + url + "' and the files it references is unknown: verify that the game may ship them",
                    "check the provider's license (for Font Awesome Free: fonts SIL OFL 1.1, icons CC BY 4.0, code MIT)");
    return result;
}

void StylesheetImporter::rewriteSheet(const std::string& baseUrl, const std::string& body,
                                      const std::string& localFile, int depth, const ImportLocation& at){
    std::vector<CssEdit> edits;

    // Prefer uncompressed fonts when supplied; otherwise retain the web-font sources.
    const std::vector<CssFontFace> faces = findFontFaces(body);
    std::vector<bool> faceHandled(faces.size(), false);
    for (size_t i = 0; i < faces.size(); i++){
        const CssFontFace& face = faces[i];
        if (!face.hasSrc)
            continue;
        faceHandled[i] = true;
        bool anyUsable = false;
        const bool hasSfnt = std::any_of(face.sources.begin(), face.sources.end(), [](const CssFontSource& source){
            return source.usable() && !source.woff();
        });
        std::string rewritten;
        for (const CssFontSource& source : face.sources){
            if (source.usable() && (!hasSfnt || !source.woff())){
                anyUsable = true;
                const std::string resolved = resolveUrl(baseUrl, source.url);
                std::string local = source.url;
                if (!skippedUrl(source.url)){
                    const std::string imported = importResource(resolved, CssUrl::Context::FontFace, localFile, depth, at);
                    if (imported.empty())
                        continue;   // failed: reported; the other candidates may still work
                    local = relativeAssetPath(localFile, imported);
                }
                if (!rewritten.empty())
                    rewritten += ", ";
                const std::string format = source.format.empty() ? (source.woff() ? urlExtension(source.url) : "truetype") : source.format;
                rewritten += cssUrl(local) + " format(\"" + format + "\")";
            }else if (source.url.empty()){
                // local(): the engine has no system fonts, but keeping it is harmless
                if (!rewritten.empty())
                    rewritten += ", ";
                rewritten += source.text;
            }
        }
        if (anyUsable && !rewritten.empty()){
            edits.push_back(CssEdit{face.srcOffset, face.srcLength, rewritten});
        }else if (!anyUsable){
            faceHandled[i] = false;
        }
    }

    for (const CssUrl& found : findCssUrls(body)){
        if (found.context == CssUrl::Context::FontFace && found.fontFace >= 0 && (size_t)found.fontFace < faceHandled.size() &&
            faceHandled[found.fontFace])
            continue;   // the whole src list was rewritten
        if (found.context == CssUrl::Context::FontFace)
            continue;   // a face without usable sources stays as written (the runtime reports it)
        if (skippedUrl(found.url))
            continue;
        const std::string resolved = resolveUrl(baseUrl, found.url);
        const std::string imported = importResource(resolved, found.context, localFile, depth, at);
        if (imported.empty())
            continue;
        edits.push_back(CssEdit{found.offset, found.length, cssUrl(relativeAssetPath(localFile, imported))});
    }

    staging.add(localFile, applyCssEdits(body, edits));
}

std::string StylesheetImporter::importResource(const std::string& url, CssUrl::Context context, const std::string& fromFile,
                                               int depth, const ImportLocation& at){
    const std::string key = ImportManifest::normalizeUrl(url);
    const auto known = done.find(key);
    if (known != done.end())
        return known->second;

    if (UrlResolver::classify(url) != UrlKind::Remote)
        return "";

    if (context == CssUrl::Context::Import && GoogleFontsImporter::isGoogleFontsUrl(url)){
        const ImportedResource nested = importer.importNested(url, "google-fonts-css", at);
        if (!nested.ok || !nested.entry()){
            done[key] = "";
            return "";
        }
        nestedFiles.push_back(nested.entry()->path);
        for (const std::string& file : nested.entry()->files)
            nestedFiles.push_back(file);
        done[key] = nested.entry()->path;
        return nested.entry()->path;
    }

    HttpResponse response;
    std::string body;
    std::string error;
    if (!importer.fetch(url, at, response, body, error)){
        done[key] = "";
        return "";
    }
    const std::string finalUrl = response.finalUrl.empty() ? url : response.finalUrl;
    const SniffedContent sniffed = sniffContent(body, response.contentType, finalUrl);

    if (context == CssUrl::Context::Import || sniffed.type == SniffedContent::Type::Css){
        if (depth + 1 > kMaxImportDepth){
            importer.report(codes::DownloadFailed, at, url, "@import chain deeper than " + std::to_string(kMaxImportDepth) +
                            " stylesheets at '" + url + "'; not followed", "flatten the stylesheet imports");
            done[key] = "";
            return "";
        }
        const std::string local = localPath(importer, url, "css");
        done[key] = local;   // a cycle finds the file it is writing
        rewriteSheet(finalUrl, body, local, depth + 1, at);
        return local;
    }

    if (!sniffed.isFont() && !sniffed.isImage()){
        importer.report(codes::UnsupportedContent, at, url, "'" + url + "' (referenced by " + fromFile + ") is " +
                        (response.contentType.empty() ? std::string(sniffed.name()) : response.contentType) +
                        ", not an image or a font; not imported");
        done[key] = "";
        return "";
    }
    if (sniffed.isImage() && !sniffed.isDecodableImage()){
        importer.report(codes::UnsupportedContent, at, url, "'" + url + "' (referenced by " + fromFile + ") is " +
                        sniffed.name() + ", which the engine cannot decode; saved, but it will not be drawn",
                        "convert the image to PNG or JPEG");
    }
    const std::string local = localPath(importer, url, sniffed.extension());
    staging.add(local, std::move(body));
    done[key] = local;
    return local;
}

ImportedResource jasperengine::editor::htmlui::importFontFile(RemoteImporter& importer, const std::string& url, const std::string& body,
                                                              const SniffedContent& sniffed, const ImportLocation& at){
    ImportedResource result;
    result.url = url;
    std::string host, path, query;
    splitUrl(url, host, path, query);
    std::string name = safeFileName(urlBaseName(url));
    if (name.empty())
        name = "font";
    const std::string local = importer.importPath("fonts/" + safeFileName(host) + "/" + urlHash(url, 12) + "-" + name + "." +
                                                  sniffed.extension());
    ImportStaging staging;
    staging.add(local, body);

    ImportEntry entry;
    entry.url = url;
    entry.kind = "font";
    entry.path = local;
    entry.files = {local};
    entry.licenseName = "unknown";
    entry.source = host;
    if (!importer.finish(staging, {entry}, result))
        return result;
    importer.report(codes::LicenseNote, at, url, "the license of the font '" + url + "' is unknown: verify that the game may ship it");
    return result;
}
