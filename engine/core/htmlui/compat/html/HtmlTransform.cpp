// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/html/HtmlTransform.h"
#include "htmlui/compat/LiteralCompare.h"
#include "htmlui/compat/StringIndex.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ImportManifest.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/css/CssRewriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

using namespace jasperengine::htmlui::compat;

TransformContext::TransformContext(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, const UrlResolver& urls):
    options(options), files(files), sink(sink), urls(urls){
}

std::string_view TransformContext::slice(SourceRef ref) const{
    return files.slice(ref);
}

void TransformContext::addHints(HtmlNode& element, const HintDeclarations& hints){
    if (hints.empty())
        return;

    if (fragment){
        // no sheet for a fragment: the hints go first in the inline style (author rules can
        // then not override them; documented in the guide)
        if (!hints.own.empty()){
            HtmlAttr* style = element.findAttr("style");
            if (style){
                style->value = hints.own + " " + style->value;
            }else{
                element.setAttr("style", hints.own);
            }
        }
        return;
    }

    const std::string key = hints.own + "|" + hints.cells;
    auto it = hintIndex.find(key);
    size_t index = 0;
    if (it == hintIndex.end()){
        index = hintRules.size();
        HintRule rule;
        rule.className = "htmlui-h" + std::to_string(index);
        rule.declarations = hints;
        hintRules.push_back(std::move(rule));
        hintIndex.emplace(key, index);
    }else{
        index = it->second;
    }
    html::addClass(element, hintRules[index].className);
}

void TransformContext::addDependency(const std::string& assetPath){
    if (assetPath.empty())
        return;
    if (std::find(dependencies.begin(), dependencies.end(), assetPath) == dependencies.end())
        dependencies.push_back(assetPath);
}

void TransformContext::addRemote(const std::string& url, const std::string& kind, SourceRef ref){
    std::string resolvedKind = kind;
    if (resolvedKind == "stylesheet" && url.find("fonts.googleapis.com") != std::string::npos)
        resolvedKind = "google-fonts-css";
    for (const RemoteReference& existing : remoteRefs){
        if (existing.url == url && existing.kind == resolvedKind)
            return;
    }
    remoteRefs.push_back(RemoteReference{url, resolvedKind, ref});
    // an import changes what the URL resolves to: the manifest is a dependency
    addDependency(ImportManifest::kPath);
}

std::string TransformContext::resolveUrl(std::string_view url, std::string_view kind, SourceRef ref, ResolvedUrl* out){
    // the same URL again (icons in a list): what resolved cleanly is reused, without touching
    // the file system; anything with a diagnostic resolves again to report it at this reference
    const std::string key = std::string(kind) + '\n' + std::string(url);
    auto known = resolvedUrls.find(key);
    ResolvedUrl resolved = (known != resolvedUrls.end()) ? known->second : urls.resolve(url, path, kind, ref, &sink);
    if (known == resolvedUrls.end()){
        const bool clean = resolved.exists && !resolved.outsideAssets && !resolved.dropped() &&
                           (resolved.kind == UrlKind::Relative || resolved.kind == UrlKind::RootRelative || resolved.kind == UrlKind::EnginePath);
        if (clean)
            resolvedUrls.emplace(key, resolved);
    }
    if (resolved.kind == UrlKind::Remote)
        addRemote(resolved.url, std::string(kind), ref);
    if (resolved.kind == UrlKind::Data && resolved.data){
        // content-addressed: the same bytes always get the same path, so they live for the process
        virtualFiles.emplace_back(resolved.assetPath, resolved.data);
    }
    if ((resolved.kind == UrlKind::Relative || resolved.kind == UrlKind::RootRelative ||
         (resolved.kind == UrlKind::Remote && resolved.entry)) && !resolved.outsideAssets){
        addDependency(resolved.assetPath);
    }
    std::string emitted = resolved.emitted;
    if (out)
        *out = std::move(resolved);
    return emitted;
}

// ---- helpers ----

std::string html::toLower(std::string_view text){
    std::string out(text);
    for (char& c : out){
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return out;
}

std::string html::trim(std::string_view text){
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' || text[begin] == '\r' || text[begin] == '\f'))
        begin++;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' || text[end - 1] == '\r' || text[end - 1] == '\f'))
        end--;
    return std::string(text.substr(begin, end - begin));
}

bool html::startsWith(std::string_view text, std::string_view prefix){
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool html::endsWith(std::string_view text, std::string_view suffix){
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool html::equalsIgnoreCase(std::string_view a, std::string_view b){
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++){
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    }
    return true;
}

std::vector<std::string> html::splitWhitespace(std::string_view text){
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < text.size()){
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == '\f'))
            i++;
        const size_t start = i;
        while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r' || text[i] == '\f'))
            i++;
        if (i > start)
            parts.emplace_back(text.substr(start, i - start));
    }
    return parts;
}

bool html::hasClass(const HtmlNode& element, std::string_view name){
    for (const std::string& part : splitWhitespace(element.attr("class"))){
        if (part == name)
            return true;
    }
    return false;
}

void html::addClass(HtmlNode& element, std::string_view name){
    if (hasClass(element, name))
        return;
    HtmlAttr* attribute = element.findAttr("class");
    if (!attribute){
        element.setAttr("class", std::string(name));
        return;
    }
    const std::string current = trim(attribute->value);
    attribute->value = current.empty() ? std::string(name) : current + " " + std::string(name);
    attribute->hasValue = true;
}

namespace {

    int hexDigit(char c){
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    struct NamedLegacyColor {
        const char* name;
        unsigned int rgb;
    };

    // CSS Color 4 named colors (the legacy parser accepts them too)
    const NamedLegacyColor namedColors[] = {
        {"aliceblue", 0xf0f8ff}, {"antiquewhite", 0xfaebd7}, {"aqua", 0x00ffff}, {"aquamarine", 0x7fffd4}, {"azure", 0xf0ffff},
        {"beige", 0xf5f5dc}, {"bisque", 0xffe4c4}, {"black", 0x000000}, {"blanchedalmond", 0xffebcd}, {"blue", 0x0000ff},
        {"blueviolet", 0x8a2be2}, {"brown", 0xa52a2a}, {"burlywood", 0xdeb887}, {"cadetblue", 0x5f9ea0}, {"chartreuse", 0x7fff00},
        {"chocolate", 0xd2691e}, {"coral", 0xff7f50}, {"cornflowerblue", 0x6495ed}, {"cornsilk", 0xfff8dc}, {"crimson", 0xdc143c},
        {"cyan", 0x00ffff}, {"darkblue", 0x00008b}, {"darkcyan", 0x008b8b}, {"darkgoldenrod", 0xb8860b}, {"darkgray", 0xa9a9a9},
        {"darkgreen", 0x006400}, {"darkgrey", 0xa9a9a9}, {"darkkhaki", 0xbdb76b}, {"darkmagenta", 0x8b008b}, {"darkolivegreen", 0x556b2f},
        {"darkorange", 0xff8c00}, {"darkorchid", 0x9932cc}, {"darkred", 0x8b0000}, {"darksalmon", 0xe9967a}, {"darkseagreen", 0x8fbc8f},
        {"darkslateblue", 0x483d8b}, {"darkslategray", 0x2f4f4f}, {"darkslategrey", 0x2f4f4f}, {"darkturquoise", 0x00ced1},
        {"darkviolet", 0x9400d3}, {"deeppink", 0xff1493}, {"deepskyblue", 0x00bfff}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
        {"dodgerblue", 0x1e90ff}, {"firebrick", 0xb22222}, {"floralwhite", 0xfffaf0}, {"forestgreen", 0x228b22}, {"fuchsia", 0xff00ff},
        {"gainsboro", 0xdcdcdc}, {"ghostwhite", 0xf8f8ff}, {"gold", 0xffd700}, {"goldenrod", 0xdaa520}, {"gray", 0x808080},
        {"green", 0x008000}, {"greenyellow", 0xadff2f}, {"grey", 0x808080}, {"honeydew", 0xf0fff0}, {"hotpink", 0xff69b4},
        {"indianred", 0xcd5c5c}, {"indigo", 0x4b0082}, {"ivory", 0xfffff0}, {"khaki", 0xf0e68c}, {"lavender", 0xe6e6fa},
        {"lavenderblush", 0xfff0f5}, {"lawngreen", 0x7cfc00}, {"lemonchiffon", 0xfffacd}, {"lightblue", 0xadd8e6}, {"lightcoral", 0xf08080},
        {"lightcyan", 0xe0ffff}, {"lightgoldenrodyellow", 0xfafad2}, {"lightgray", 0xd3d3d3}, {"lightgreen", 0x90ee90}, {"lightgrey", 0xd3d3d3},
        {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a}, {"lightseagreen", 0x20b2aa}, {"lightskyblue", 0x87cefa}, {"lightslategray", 0x778899},
        {"lightslategrey", 0x778899}, {"lightsteelblue", 0xb0c4de}, {"lightyellow", 0xffffe0}, {"lime", 0x00ff00}, {"limegreen", 0x32cd32},
        {"linen", 0xfaf0e6}, {"magenta", 0xff00ff}, {"maroon", 0x800000}, {"mediumaquamarine", 0x66cdaa}, {"mediumblue", 0x0000cd},
        {"mediumorchid", 0xba55d3}, {"mediumpurple", 0x9370db}, {"mediumseagreen", 0x3cb371}, {"mediumslateblue", 0x7b68ee},
        {"mediumspringgreen", 0x00fa9a}, {"mediumturquoise", 0x48d1cc}, {"mediumvioletred", 0xc71585}, {"midnightblue", 0x191970},
        {"mintcream", 0xf5fffa}, {"mistyrose", 0xffe4e1}, {"moccasin", 0xffe4b5}, {"navajowhite", 0xffdead}, {"navy", 0x000080},
        {"oldlace", 0xfdf5e6}, {"olive", 0x808000}, {"olivedrab", 0x6b8e23}, {"orange", 0xffa500}, {"orangered", 0xff4500},
        {"orchid", 0xda70d6}, {"palegoldenrod", 0xeee8aa}, {"palegreen", 0x98fb98}, {"paleturquoise", 0xafeeee}, {"palevioletred", 0xdb7093},
        {"papayawhip", 0xffefd5}, {"peachpuff", 0xffdab9}, {"peru", 0xcd853f}, {"pink", 0xffc0cb}, {"plum", 0xdda0dd},
        {"powderblue", 0xb0e0e6}, {"purple", 0x800080}, {"rebeccapurple", 0x663399}, {"red", 0xff0000}, {"rosybrown", 0xbc8f8f},
        {"royalblue", 0x4169e1}, {"saddlebrown", 0x8b4513}, {"salmon", 0xfa8072}, {"sandybrown", 0xf4a460}, {"seagreen", 0x2e8b57},
        {"seashell", 0xfff5ee}, {"sienna", 0xa0522d}, {"silver", 0xc0c0c0}, {"skyblue", 0x87ceeb}, {"slateblue", 0x6a5acd},
        {"slategray", 0x708090}, {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"springgreen", 0x00ff7f}, {"steelblue", 0x4682b4},
        {"tan", 0xd2b48c}, {"teal", 0x008080}, {"thistle", 0xd8bfd8}, {"tomato", 0xff6347}, {"turquoise", 0x40e0d0},
        {"violet", 0xee82ee}, {"wheat", 0xf5deb3}, {"white", 0xffffff}, {"whitesmoke", 0xf5f5f5}, {"yellow", 0xffff00},
        {"yellowgreen", 0x9acd32},
    };

    std::string hexColor(unsigned int r, unsigned int g, unsigned int b){
        static const char digits[] = "0123456789abcdef";
        std::string out = "#";
        for (unsigned int channel : {r, g, b}){
            out += digits[(channel >> 4) & 15];
            out += digits[channel & 15];
        }
        return out + "ff";
    }

}

// HTML Living Standard §2.3.6 "rules for parsing a legacy colour value"
std::string html::legacyColor(std::string_view input){
    std::string value = trim(input);
    if (value.empty() || equalsIgnoreCase(value, "transparent"))
        return "";

    const std::string lower = toLower(value);
    for (const NamedLegacyColor& named : namedColors){
        if (lower == named.name)
            return hexColor((named.rgb >> 16) & 255, (named.rgb >> 8) & 255, named.rgb & 255);
    }

    if (value.size() == 4 && value[0] == '#' && hexDigit(value[1]) >= 0 && hexDigit(value[2]) >= 0 && hexDigit(value[3]) >= 0){
        return hexColor((unsigned)hexDigit(value[1]) * 17, (unsigned)hexDigit(value[2]) * 17, (unsigned)hexDigit(value[3]) * 17);
    }

    // characters above U+FFFF count as "00" (byte-wise: every non-ASCII byte is replaced)
    std::string digits;
    for (char c : value){
        digits += ((unsigned char)c >= 0x80) ? '0' : c;
    }
    if (digits.size() > 128)
        digits.resize(128);
    if (!digits.empty() && digits[0] == '#')
        digits.erase(0, 1);
    for (char& c : digits){
        if (hexDigit(c) < 0)
            c = '0';
    }
    while (digits.empty() || digits.size() % 3 != 0)
        digits += '0';
    size_t length = digits.size() / 3;
    std::string components[3] = {digits.substr(0, length), digits.substr(length, length), digits.substr(2 * length, length)};
    if (length > 8){
        for (std::string& component : components)
            component = component.substr(length - 8);
        length = 8;
    }
    while (length > 2 && components[0][0] == '0' && components[1][0] == '0' && components[2][0] == '0'){
        for (std::string& component : components)
            component.erase(0, 1);
        length--;
    }
    if (length > 2){
        for (std::string& component : components)
            component.resize(2);
        length = 2;
    }
    unsigned int channels[3];
    for (int i = 0; i < 3; i++){
        unsigned int channel = 0;
        for (char c : components[i])
            channel = channel * 16 + (unsigned)hexDigit(c);
        channels[i] = channel;
    }
    return hexColor(channels[0], channels[1], channels[2]);
}

bool html::parseDimension(std::string_view input, double& number, bool& percent){
    const std::string value = trim(input);
    percent = false;
    if (value.empty() || !(std::isdigit((unsigned char)value[0]) || value[0] == '.'))
        return false;
    size_t i = 0;
    while (i < value.size() && (std::isdigit((unsigned char)value[i]) || value[i] == '.'))
        i++;
    number = std::strtod(value.substr(0, i).c_str(), nullptr);
    if (i < value.size() && value[i] == '%')
        percent = true;
    return std::isfinite(number);
}

std::string html::kebabCase(std::string_view name){
    std::string out;
    for (size_t i = 0; i < name.size(); i++){
        const char c = name[i];
        const bool upper = c >= 'A' && c <= 'Z';
        const bool digit = c >= '0' && c <= '9';
        if (i > 0){
            const char previous = name[i - 1];
            const bool previousLower = previous >= 'a' && previous <= 'z';
            const bool previousDigit = previous >= '0' && previous <= '9';
            const bool previousLetter = previousLower || (previous >= 'A' && previous <= 'Z');
            if ((upper && (previousLower || previousDigit)) || (digit && previousLetter))
                out += '-';
        }
        out += upper ? (char)(c - 'A' + 'a') : c;
    }
    return out;
}

const ElementSupport* html::elementRow(const std::string& tag){
    // per thread: compiles may run on several
    thread_local NameTable<const ElementSupport*> rows;
    return rows.get(tag, [](std::string_view name){ return findElement(name); });
}

const AttributeSupport* html::attributeRow(const std::string& tag, const std::string& name){
    thread_local std::unordered_map<std::string, const AttributeSupport*> rows;
    std::string key = tag;
    key += '\n';
    key += name;
    auto it = rows.find(key);
    if (it != rows.end())
        return it->second;
    const AttributeSupport* row = findAttribute(tag, name);
    rows.emplace(std::move(key), row);
    return row;
}

// ---- the pipeline ----

void jasperengine::htmlui::compat::runTransforms(TransformContext& context){
    if (!context.root)
        return;

    if (!context.fragment){
        transformHead(context);
    }
    // scripts and styles of fragments (and the body's of documents) go here too
    transformScripts(context, *context.root);
    transformReservedNames(context, *context.root);
    if (!context.fragment){
        transformRoot(context);
    }
    transformTemplates(context, *context.root);

    transformElements(context, *context.root);
    transformLucide(context, *context.root);
    transformForms(context, *context.root);
    transformAttributes(context, *context.root);

    // template contents get the element passes (their scripts and styles are inert)
    for (TemplateSource& source : context.templates){
        if (!source.content)
            continue;
        transformElements(context, *source.content);
        transformLucide(context, *source.content);
        transformForms(context, *source.content);
        transformAttributes(context, *source.content, source.inModel);
    }
}
