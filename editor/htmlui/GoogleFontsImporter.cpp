// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GoogleFontsImporter.h"

#include "ContentSniffer.h"
#include "StylesheetImporter.h"

#include "htmlui/compat/Hash.h"
#include "htmlui/compat/UrlResolver.h"

#include "json.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <set>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using namespace jasperengine::htmlui::compat;

namespace {

    using Variant = GoogleFontsImporter::Variant;
    using FamilyRequest = GoogleFontsImporter::FamilyRequest;

    std::string trim(const std::string& text){
        size_t begin = 0, end = text.size();
        while (begin < end && (unsigned char)text[begin] <= ' ')
            begin++;
        while (end > begin && (unsigned char)text[end - 1] <= ' ')
            end--;
        return text.substr(begin, end - begin);
    }

    std::vector<std::string> split(const std::string& text, char separator){
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= text.size()){
            size_t end = text.find(separator, start);
            if (end == std::string::npos)
                end = text.size();
            parts.push_back(text.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    }

    bool parseInt(const std::string& text, int& value){
        if (text.empty())
            return false;
        for (char c : text){
            if (c < '0' || c > '9')
                return false;
        }
        value = std::atoi(text.c_str());
        return true;
    }

    // a legacy css API variant or a facade spec variant: 400, 700i, 700italic, italic, bold,
    // bolditalic, regular, b, i, bi
    bool parseVariant(std::string text, Variant& variant){
        text = lowerAscii(trim(text));
        if (text.empty())
            return false;
        variant = Variant();
        if (text == "regular" || text == "r" || text == "normal"){
            return true;
        }
        if (text == "italic" || text == "i"){
            variant.italic = true;
            return true;
        }
        if (text == "bold" || text == "b"){
            variant.weight = 700;
            return true;
        }
        if (text == "bolditalic" || text == "bi"){
            variant.weight = 700;
            variant.italic = true;
            return true;
        }
        size_t digits = 0;
        while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9')
            digits++;
        if (digits == 0 || !parseInt(text.substr(0, digits), variant.weight))
            return false;
        const std::string rest = text.substr(digits);
        if (rest == "i" || rest == "italic")
            variant.italic = true;
        else if (!rest.empty())
            return false;
        return variant.weight >= 1 && variant.weight <= 1000;
    }

    // css2 "ital,wght@0,400;1,700" / "wght@100..900" applied to request
    bool parseAxes(const std::string& spec, FamilyRequest& request){
        const size_t at = spec.find('@');
        if (at == std::string::npos)
            return false;
        const std::vector<std::string> axes = split(lowerAscii(spec.substr(0, at)), ',');
        for (const std::string& tuple : split(spec.substr(at + 1), ';')){
            const std::vector<std::string> values = split(tuple, ',');
            if (values.size() != axes.size())
                return false;
            Variant variant;
            int rangeMin = 0, rangeMax = 0;
            for (size_t i = 0; i < axes.size(); i++){
                const std::string value = trim(values[i]);
                if (axes[i] == "ital"){
                    variant.italic = value == "1";
                }else if (axes[i] == "wght"){
                    const size_t dots = value.find("..");
                    if (dots != std::string::npos){
                        if (!parseInt(value.substr(0, dots), rangeMin) || !parseInt(value.substr(dots + 2), rangeMax))
                            return false;
                    }else if (!parseInt(value, variant.weight)){
                        return false;
                    }
                }
                // other axes (wdth, opsz, …) select nothing static
            }
            if (rangeMax > 0){
                request.rangeMin = rangeMin;
                request.rangeMax = rangeMax;
                request.rangeItalic = request.rangeItalic || variant.italic;
            }else{
                request.variants.push_back(variant);
            }
        }
        std::sort(request.variants.begin(), request.variants.end());
        request.variants.erase(std::unique(request.variants.begin(), request.variants.end()), request.variants.end());
        return true;
    }

    std::string queryValueDecode(const std::string& text){
        std::string spaced = text;
        std::replace(spaced.begin(), spaced.end(), '+', ' ');
        return UrlResolver::percentDecode(spaced);
    }

    size_t editDistance(const std::string& a, const std::string& b){
        std::vector<size_t> previous(b.size() + 1), current(b.size() + 1);
        for (size_t j = 0; j <= b.size(); j++)
            previous[j] = j;
        for (size_t i = 1; i <= a.size(); i++){
            current[0] = i;
            for (size_t j = 1; j <= b.size(); j++){
                const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
                current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
            }
            previous.swap(current);
        }
        return previous[b.size()];
    }

    std::string variantName(const Variant& variant){
        return std::to_string(variant.weight) + (variant.italic ? " italic" : "");
    }

    std::string familyList(const std::vector<std::string>& families){
        std::string text;
        for (size_t i = 0; i < families.size(); i++){
            if (i > 0)
                text += ", ";
            text += "'" + families[i] + "'";
        }
        return text;
    }

    struct MetadataCache {
        std::mutex mutex;
        std::map<std::string, std::shared_ptr<void>> entries;
    };

    MetadataCache& metadataCache(){
        static MetadataCache cache;
        return cache;
    }

}

GoogleFontsImporter::GoogleFontsImporter(RemoteImporter& importer): importer(importer) {}

GoogleFontsImporter::GoogleFontsImporter(RemoteImporter& importer, Options options): importer(importer), options(std::move(options)) {}

std::string GoogleFontsImporter::fontPath(const std::string& relative) const{
    return options.dir.empty() ? importer.importPath("fonts/" + relative) : options.dir + "/" + relative;
}

bool GoogleFontsImporter::isGoogleFontsUrl(const std::string& url){
    std::string host, path, query;
    if (!splitUrl(url, host, path, query))
        return false;
    return host == "fonts.googleapis.com" && (path == "/css" || path == "/css2" || path == "/icon");
}

std::vector<FamilyRequest> GoogleFontsImporter::parseRequest(const std::string& url){
    std::vector<FamilyRequest> families;
    std::string host, path, query;
    if (!splitUrl(url, host, path, query))
        return families;
    const bool css2 = path == "/css2";
    for (const std::string& parameter : split(query, '&')){
        if (parameter.compare(0, 7, "family=") != 0)
            continue;
        const std::string value = queryValueDecode(parameter.substr(7));
        // the legacy API takes several families in one parameter: "Roboto:400,700|Open Sans"
        for (const std::string& item : css2 ? std::vector<std::string>{value} : split(value, '|')){
            FamilyRequest request;
            const size_t colon = item.find(':');
            request.family = trim(item.substr(0, colon));
            if (request.family.empty())
                continue;
            if (colon != std::string::npos){
                const std::string spec = item.substr(colon + 1);
                if (spec.find('@') != std::string::npos){
                    parseAxes(spec, request);
                }else{
                    for (const std::string& variantText : split(spec, ',')){
                        Variant variant;
                        if (parseVariant(variantText, variant))
                            request.variants.push_back(variant);
                    }
                    std::sort(request.variants.begin(), request.variants.end());
                    request.variants.erase(std::unique(request.variants.begin(), request.variants.end()), request.variants.end());
                }
            }
            families.push_back(std::move(request));
        }
    }
    return families;
}

bool GoogleFontsImporter::parseSpec(const std::string& spec, FamilyRequest& request, std::string& error){
    request = FamilyRequest();
    const size_t colon = spec.find(':');
    request.family = trim(spec.substr(0, colon));
    if (request.family.empty()){
        error = "no family name in '" + spec + "' (write it like \"Inter:400,700\")";
        return false;
    }
    if (colon == std::string::npos)
        return true;
    const std::string variants = trim(spec.substr(colon + 1));
    if (variants.find('@') != std::string::npos){
        if (!parseAxes(variants, request)){
            error = "cannot read the axes of '" + spec + "' (write them like \"ital,wght@0,400;1,700\")";
            return false;
        }
        return true;
    }
    for (const std::string& text : split(variants, ',')){
        const std::string item = trim(text);
        if (item.empty())
            continue;
        const size_t dots = item.find("..");
        if (dots != std::string::npos){
            if (!parseInt(item.substr(0, dots), request.rangeMin) || !parseInt(item.substr(dots + 2), request.rangeMax) ||
                request.rangeMin > request.rangeMax){
                error = "cannot read the weight range '" + item + "' of '" + spec + "' (write it like 100..900)";
                return false;
            }
            continue;
        }
        Variant variant;
        if (!parseVariant(item, variant)){
            error = "cannot read the variant '" + item + "' of '" + spec + "' (weights like 400, 700i, 700italic)";
            return false;
        }
        request.variants.push_back(variant);
    }
    std::sort(request.variants.begin(), request.variants.end());
    request.variants.erase(std::unique(request.variants.begin(), request.variants.end()), request.variants.end());
    return true;
}

std::string GoogleFontsImporter::css2Url(const std::vector<FamilyRequest>& families){
    std::string url = "https://fonts.googleapis.com/css2";
    char separator = '?';
    for (const FamilyRequest& request : families){
        std::string name = request.family;
        std::replace(name.begin(), name.end(), ' ', '+');
        url += separator;
        url += "family=" + name;
        separator = '&';

        bool italic = request.rangeMax > 0 && request.rangeItalic;
        for (const Variant& variant : request.variants)
            italic = italic || variant.italic;
        const std::string range = request.rangeMax > 0 ? std::to_string(request.rangeMin) + ".." + std::to_string(request.rangeMax) : "";
        if (request.variants.empty() && range.empty())
            continue;

        // Google rejects tuples out of order: normal before italic, then by weight
        std::vector<std::string> tuples;
        if (italic){
            if (!range.empty())
                tuples.push_back("0," + range);
            for (const Variant& variant : request.variants){
                if (!variant.italic)
                    tuples.push_back("0," + std::to_string(variant.weight));
            }
            if (!range.empty() && request.rangeItalic)
                tuples.push_back("1," + range);
            for (const Variant& variant : request.variants){
                if (variant.italic)
                    tuples.push_back("1," + std::to_string(variant.weight));
            }
            url += ":ital,wght@";
        }else{
            if (!range.empty())
                tuples.push_back(range);
            for (const Variant& variant : request.variants)
                tuples.push_back(std::to_string(variant.weight));
            url += ":wght@";
        }
        for (size_t i = 0; i < tuples.size(); i++){
            if (i > 0)
                url += ';';
            url += tuples[i];
        }
    }
    return url;
}

std::string GoogleFontsImporter::familySlug(const std::string& family){
    std::string slug;
    bool dash = false;
    for (unsigned char c : family){
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')){
            slug += (char)c;
            dash = false;
        }else if (c >= 'A' && c <= 'Z'){
            slug += (char)(c - 'A' + 'a');
            dash = false;
        }else if (!slug.empty() && !dash){
            slug += '-';
            dash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-')
        slug.pop_back();
    return slug.empty() ? "font" : slug;
}

std::string GoogleFontsImporter::familyFileStem(const std::string& family){
    std::string stem;
    for (unsigned char c : family){
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            stem += (char)c;
    }
    return stem.empty() ? "Font" : stem;
}

std::string GoogleFontsImporter::licenseUrl(const std::string& family){
    std::string encoded;
    for (unsigned char c : family){
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'){
            encoded += (char)c;
        }else{
            static const char* hex = "0123456789ABCDEF";
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 15];
        }
    }
    return "https://fonts.google.com/download/list?family=" + encoded;
}

std::string GoogleFontsImporter::metadataUrl(){
    return "https://fonts.google.com/metadata/fonts";
}

std::string GoogleFontsImporter::licenseNameForFile(const std::string& fileName){
    const std::string name = lowerAscii(fileName);
    if (name.find("ofl") != std::string::npos)
        return "SIL Open Font License 1.1";
    if (name.find("ufl") != std::string::npos)
        return "Ubuntu Font License 1.0";
    if (name.find("license") != std::string::npos || name.find("apache") != std::string::npos)
        return "Apache License 2.0";
    return "unknown";
}

std::string GoogleFontsImporter::closestFamily(const std::string& family, const std::vector<std::string>& families){
    const std::string wanted = lowerAscii(family);
    std::string best;
    size_t bestDistance = std::string::npos;
    for (const std::string& candidate : families){
        const size_t distance = editDistance(wanted, lowerAscii(candidate));
        if (distance < bestDistance || (distance == bestDistance && candidate.size() < best.size())){
            best = candidate;
            bestDistance = distance;
        }
    }
    if (best.empty() || bestDistance > std::max<size_t>(3, wanted.size() / 2))
        return "";
    return best;
}

void GoogleFontsImporter::clearSessionCache(){
    std::lock_guard<std::mutex> lock(metadataCache().mutex);
    metadataCache().entries.clear();
}

const GoogleFontsImporter::Metadata* GoogleFontsImporter::metadata(const ImportLocation& at){
    std::string key = HttpClient::mirrorDirectory() + "|";
    for (const auto& pair : importer.http().getHostRewrite())
        key += pair.first + ">" + pair.second + "|";

    MetadataCache& cache = metadataCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto it = cache.entries.find(key);
        if (it != cache.entries.end()){
            const Metadata* known = static_cast<const Metadata*>(it->second.get());
            return known->loaded ? known : nullptr;
        }
    }

    auto loaded = std::make_shared<Metadata>();
    HttpResponse response;
    std::string body;
    std::string error;
    if (importer.fetch(metadataUrl(), at, response, body, error, true)){
        // the JSON may carry the ")]}'" guard line
        if (body.compare(0, 4, ")]}'") == 0){
            const size_t newline = body.find('\n');
            body.erase(0, newline == std::string::npos ? body.size() : newline + 1);
        }
        const nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
        const auto list = json.is_object() ? json.find("familyMetadataList") : json.end();
        if (json.is_object() && list != json.end() && list->is_array()){
            for (const nlohmann::json& item : *list){
                if (!item.is_object() || !item.contains("family") || !item["family"].is_string())
                    continue;
                const std::string family = item["family"].get<std::string>();
                loaded->families.push_back(family);
                std::vector<Variant>& variants = loaded->variants[lowerAscii(family)];
                const auto fonts = item.find("fonts");
                if (fonts != item.end() && fonts->is_object()){
                    for (const auto& font : fonts->items()){
                        Variant variant;
                        if (parseVariant(font.key(), variant))
                            variants.push_back(variant);
                    }
                }
                std::sort(variants.begin(), variants.end());
            }
            loaded->loaded = !loaded->families.empty();
        }
    }

    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.entries[key] = loaded;
    return loaded->loaded ? loaded.get() : nullptr;
}

ImportedResource GoogleFontsImporter::run(const std::string& url, const ImportLocation& at){
    ImportedResource result;
    result.url = url;

    std::string host, path, query;
    splitUrl(url, host, path, query);
    std::vector<FamilyRequest> requested = parseRequest(url);
    std::string requestUrl = url;

    HttpResponse response;
    std::string css;
    std::string error;
    if (!importer.fetch(requestUrl, at, response, css, error, true)){
        if (error == "the curl executable was not found"){
            result.error = error;
            return result;
        }
        if (response.status != 400){
            result.error = error;
            importer.report(codes::DownloadFailed, at, url, "download of the Google Fonts stylesheet '" + url + "' failed: " + error,
                            "check the network, then import again (Inspector: Import, or jasperengine-editor import-ui)");
            return result;
        }

        // 400: an unknown family, or a weight/style the family does not have
        const Metadata* known = metadata(at);
        if (!known || requested.empty()){
            result.error = "Google Fonts answered HTTP 400 (unknown family or style)";
            importer.report(codes::DownloadFailed, at, url, "Google Fonts does not know a family or style of '" + url + "' (HTTP 400)",
                            "check the family names and weights on https://fonts.google.com");
            return result;
        }

        bool unknownFamily = false;
        std::vector<FamilyRequest> adjusted;
        for (FamilyRequest request : requested){
            const auto it = known->variants.find(lowerAscii(request.family));
            if (it == known->variants.end()){
                unknownFamily = true;
                const std::string closest = closestFamily(request.family, known->families);
                importer.report(codes::DownloadFailed, at, url, "unknown Google Fonts family '" + request.family + "'",
                                closest.empty() ? std::string("check the name on https://fonts.google.com")
                                                : "did you mean '" + closest + "'? (family=" + [&closest]{
                                                      std::string name = closest;
                                                      std::replace(name.begin(), name.end(), ' ', '+');
                                                      return name;
                                                  }() + ")");
                continue;
            }
            const std::vector<Variant>& offered = it->second;
            std::vector<Variant> kept;
            for (const Variant& variant : request.variants){
                if (std::find(offered.begin(), offered.end(), variant) != offered.end()){
                    kept.push_back(variant);
                    continue;
                }
                // the nearest the family has (the engine synthesizes bold and italic from it)
                Variant nearest = offered.empty() ? Variant() : offered.front();
                int bestScore = 1 << 30;
                for (const Variant& candidate : offered){
                    const int score = std::abs(candidate.weight - variant.weight) + (candidate.italic != variant.italic ? 10000 : 0);
                    if (score < bestScore){
                        bestScore = score;
                        nearest = candidate;
                    }
                }
                std::string offeredText;
                for (const Variant& candidate : offered)
                    offeredText += (offeredText.empty() ? "" : ", ") + variantName(candidate);
                importer.report(codes::FontVariantUnavailable, at, url,
                                "Google Fonts family '" + request.family + "' has no " + variantName(variant) + " (it offers " +
                                offeredText + "); imported " + variantName(nearest) + " instead",
                                "use one of the offered weights, or keep it: the engine synthesizes bold and italic");
                if (std::find(kept.begin(), kept.end(), nearest) == kept.end())
                    kept.push_back(nearest);
            }
            std::sort(kept.begin(), kept.end());
            request.variants = kept;
            if (request.rangeMax > 0){
                // a range keeps what the family has inside it
                bool any = false;
                for (const Variant& candidate : offered)
                    any = any || (candidate.weight >= request.rangeMin && candidate.weight <= request.rangeMax);
                if (!any){
                    request.rangeMin = request.rangeMax = 0;
                    if (request.variants.empty() && !offered.empty())
                        request.variants.push_back(offered.front());
                }
            }
            adjusted.push_back(request);
        }
        if (unknownFamily || adjusted.empty()){
            result.error = "unknown Google Fonts family";
            return result;
        }
        requestUrl = css2Url(adjusted);
        if (!importer.fetch(requestUrl, at, response, css, error, true)){
            result.error = error;
            importer.report(codes::DownloadFailed, at, url, "download of the Google Fonts stylesheet '" + requestUrl + "' failed: " + error,
                            "check the family names and weights on https://fonts.google.com");
            return result;
        }
    }

    const std::vector<CssFontFace> faces = findFontFaces(css);
    if (faces.empty()){
        result.error = "the Google Fonts response has no @font-face rule";
        importer.report(codes::UnsupportedContent, at, url, "the Google Fonts response for '" + url + "' has no @font-face rule (" +
                        (response.contentType.empty() ? std::string("no content type") : response.contentType) + ")");
        return result;
    }

    // the legacy API leaves out what a family does not have, without an error
    if (path == "/css"){
        for (const FamilyRequest& request : requested){
            for (const Variant& variant : request.variants){
                bool found = false;
                for (const CssFontFace& face : faces)
                    found = found || (lowerAscii(face.family) == lowerAscii(request.family) && face.weight == variant.weight &&
                                      face.italic == variant.italic);
                if (!found)
                    importer.report(codes::FontVariantUnavailable, at, url,
                                    "Google Fonts family '" + request.family + "' has no " + variantName(variant) + "; not imported",
                                    "use a weight the family offers (see https://fonts.google.com), or keep it: the engine synthesizes bold and italic");
            }
        }
    }

    ImportStaging staging;
    std::vector<std::string> families;
    std::vector<std::string> fontFiles;
    std::vector<CssEdit> edits;
    std::set<std::string> usedNames;
    for (const CssFontFace& face : faces){
        const CssFontSource* source = nullptr;
        for (const CssFontSource& candidate : face.sources){
            if (candidate.usable() && (!source || (source->woff() && !candidate.woff())))
                source = &candidate;
        }
        if (!source){
            for (const CssFontSource& candidate : face.sources){
                if (!candidate.url.empty()){
                    source = &candidate;
                    break;
                }
            }
        }
        if (!source || !face.hasSrc){
            result.error = "a Google Fonts @font-face has no font file";
            importer.report(codes::UnsupportedContent, at, url,
                            "@font-face '" + face.family + "' of '" + url + "' has no supported font source");
            return result;
        }

        HttpResponse fontResponse;
        std::string font;
        const std::string fontUrl = resolveUrl(response.finalUrl.empty() ? requestUrl : response.finalUrl, source->url);
        if (!importer.fetch(fontUrl, at, fontResponse, font, error)){
            result.error = error;
            return result;
        }
        const SniffedContent sniffed = sniffContent(font, fontResponse.contentType, fontUrl);
        if (!sniffed.isFont()){
            result.error = std::string("Google Fonts sent a ") + sniffed.name() + " instead of a font";
            importer.report(codes::UnsupportedContent,
                            at, fontUrl, "'" + fontUrl + "' (" + face.family + " " + std::to_string(face.weight) + ") is a " + sniffed.name() +
                            ", not a supported font; nothing imported");
            return result;
        }

        const std::string slug = familySlug(face.family);
        std::string stem = familyFileStem(face.family) + "-" + std::to_string(face.weight) + (face.italic ? "-italic" : "");
        std::string fileName = slug + "/" + stem + "." + sniffed.extension();
        for (int n = 2; usedNames.count(fileName); n++)
            fileName = slug + "/" + stem + "-" + std::to_string(n) + "." + sniffed.extension();
        usedNames.insert(fileName);

        const std::string local = fontPath(fileName);
        staging.add(local, std::move(font));
        fontFiles.push_back(local);
        edits.push_back(CssEdit{face.srcOffset, face.srcLength, cssUrl(fileName) + " format(\"" +
                                (sniffed.type == SniffedContent::Type::OpenType ? "opentype" :
                                 sniffed.type == SniffedContent::Type::Woff ? "woff" :
                                 sniffed.type == SniffedContent::Type::Woff2 ? "woff2" : "truetype") + "\")"});
        if (std::find(families.begin(), families.end(), face.family) == families.end())
            families.push_back(face.family);
    }

    std::string cssStem;
    for (size_t i = 0; i < families.size() && i < 3; i++)
        cssStem += (cssStem.empty() ? "" : "-") + familySlug(families[i]);
    if (families.size() > 3)
        cssStem += "-more";
    cssStem += "-" + urlHash(url, 6);
    const std::string cssPath = importer.importPath("fonts/" + cssStem + ".css");
    if (options.writeCss)
        staging.add(cssPath, applyCssEdits(css, edits));

    // licenses, one file per family; a failure only leaves the license unknown
    std::vector<std::pair<std::string, std::string>> licenses;   // family, asset path
    std::set<std::string> licenseNames;
    std::string combined;
    for (const std::string& family : families){
        HttpResponse licenseResponse;
        std::string body;
        std::string licenseError;
        bool saved = false;
        if (importer.fetch(licenseUrl(family), at, licenseResponse, body, licenseError, true)){
            if (body.compare(0, 4, ")]}'") == 0){
                const size_t newline = body.find('\n');
                body.erase(0, newline == std::string::npos ? body.size() : newline + 1);
            }
            const nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
            const nlohmann::json* files = nullptr;
            if (json.is_object() && json.contains("manifest") && json["manifest"].is_object() && json["manifest"].contains("files") &&
                json["manifest"]["files"].is_array())
                files = &json["manifest"]["files"];
            if (files){
                for (const nlohmann::json& file : *files){
                    if (!file.is_object() || !file.contains("filename") || !file.contains("contents") || !file["filename"].is_string() ||
                        !file["contents"].is_string())
                        continue;
                    const std::string fileName = safeFileName(file["filename"].get<std::string>());
                    const std::string lowered = lowerAscii(fileName);
                    if (lowered.find("ofl") == std::string::npos && lowered.find("license") == std::string::npos &&
                        lowered.find("ufl") == std::string::npos)
                        continue;
                    const std::string local = fontPath(familySlug(family) + "/" + fileName);
                    const std::string contents = file["contents"].get<std::string>();
                    staging.add(local, contents);
                    licenses.emplace_back(family, local);
                    licenseNames.insert(licenseNameForFile(fileName));
                    combined += "==== " + family + " (" + local + ") ====\n\n" + contents + "\n\n";
                    saved = true;
                    break;
                }
            }
            if (!saved)
                licenseError = "the download list has no license file";
        }
        if (!saved){
            importer.report(codes::LicenseNote, at, url, "the license of Google Fonts family '" + family + "' could not be fetched (" +
                            licenseError + "); it is unknown in the manifest",
                            "Google Fonts are licensed under the SIL Open Font License, Apache 2.0 or the Ubuntu Font License: "
                            "check https://fonts.google.com/specimen/" + [&family]{
                                std::string name = family;
                                std::replace(name.begin(), name.end(), ' ', '+');
                                return name;
                            }() + "/license");
        }
    }

    if (!options.writeManifest){
        // fonts-only: the font files and each family's licence, nothing that names them
        std::string committed;
        if (options.staging){
            for (const auto& file : staging.files()){
                options.staging->add(file.first, file.second);
                result.written.push_back(file.first);
            }
        }else if (!staging.commit(importer.target(), result.written, committed)){
            result.error = committed;
            importer.report(codes::DownloadFailed, at, url, "import of '" + url + "' failed: " + committed);
            return result;
        }
        result.ok = true;
        if (!licenses.empty()){
            std::string licenseFiles;
            for (const auto& license : licenses)
                licenseFiles += (licenseFiles.empty() ? "" : ", ") + license.second;
            importer.report(codes::LicenseNote, at, url, "Google Fonts " + familyList(families) + ": " + [&licenseNames]{
                                std::string names;
                                for (const std::string& name : licenseNames)
                                    names += (names.empty() ? "" : ", ") + name;
                                return names;
                            }() + " (" + licenseFiles + "); the license file ships with the game in the assets");
        }
        return result;
    }

    ImportEntry entry;
    entry.url = url;
    entry.kind = "google-fonts-css";
    entry.path = cssPath;
    entry.files = fontFiles;
    for (const auto& license : licenses)
        entry.files.push_back(license.second);
    if (licenses.size() == 1 && families.size() == 1){
        entry.license = licenses[0].second;
    }else if (!licenses.empty()){
        entry.license = importer.importPath("fonts/" + cssStem + ".licenses.txt");
        staging.add(entry.license, combined);
        entry.files.push_back(entry.license);
    }
    if (licenseNames.empty() || licenses.size() < families.size())
        licenseNames.insert("unknown");
    for (const std::string& name : licenseNames)
        entry.licenseName += (entry.licenseName.empty() ? "" : ", ") + name;
    entry.source = "Google Fonts";

    if (!importer.finish(staging, {entry}, result))
        return result;
    if (!licenses.empty()){
        importer.report(codes::LicenseNote, at, url, "Google Fonts " + familyList(families) + ": " + entry.licenseName + " (" +
                        entry.license + "); the license file ships with the game in the assets");
    }
    return result;
}
