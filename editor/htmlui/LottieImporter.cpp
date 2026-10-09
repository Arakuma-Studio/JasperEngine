// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LottieImporter.h"

#include "ContentSniffer.h"

#include "htmlui/vector/DotLottie.h"

#include "json.hpp"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor::htmlui;
using namespace jasperengine::htmlui::compat;

namespace {

    bool isLottieFilesHost(const std::string& host){
        auto endsWith = [&host](const std::string& suffix){
            return host.size() >= suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        return host == "lottiefiles.com" || endsWith(".lottiefiles.com") || host == "lottie.host" || endsWith(".lottie.host");
    }

    // "images/img_0.png" -> "img_0.png"
    std::string assetFileName(const std::string& path){
        const size_t slash = path.find_last_of('/');
        return safeFileName(slash == std::string::npos ? path : path.substr(slash + 1));
    }

}

LottieImporter::LottieImporter(RemoteImporter& importer): importer(importer) {}

bool LottieImporter::looksLikeLottie(const std::string& json){
    const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
    if (!parsed.is_object())
        return false;
    const auto layers = parsed.find("layers");
    return layers != parsed.end() && layers->is_array() && (parsed.contains("v") || parsed.contains("fr") || parsed.contains("ip"));
}

bool LottieImporter::isLottieFilesPage(const std::string& url){
    std::string host, path, query;
    if (!splitUrl(url, host, path, query))
        return false;
    return host == "lottiefiles.com" || host == "www.lottiefiles.com" || host == "app.lottiefiles.com";
}

std::vector<std::string> LottieImporter::findAnimationUrls(const std::string& page){
    // pages carry URLs in scripts as https://...
    std::string html = page;
    for (const char* escaped : {"\\u002F", "\\u002f", "\\/"}){
        const std::string pattern(escaped);
        size_t at = 0;
        while ((at = html.find(pattern, at)) != std::string::npos){
            html.replace(at, pattern.size(), "/");
            at++;
        }
    }

    std::vector<std::string> urls;
    std::set<std::string> seen;
    size_t search = 0;
    while ((search = html.find("https://", search)) != std::string::npos){
        // a URL inside another URL's query (?lottieUrl=https%3A...) is percent-encoded and not found here
        size_t end = search + 8;
        while (end < html.size()){
            const char c = html[end];
            if (c == '"' || c == '\'' || c == '<' || c == '>' || c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ')' ||
                c == '(' || c == '\\' || c == '?' || c == '#' || c == '&' || c == ',')
                break;
            end++;
        }
        const std::string candidate = html.substr(search, end - search);
        search = end;
        const std::string extension = urlExtension(candidate);
        if (extension != "json" && extension != "lottie")
            continue;
        std::string host, path, query;
        if (!splitUrl(candidate, host, path, query) || !isLottieFilesHost(host))
            continue;
        if (seen.insert(candidate).second)
            urls.push_back(candidate);
    }
    return urls;
}

std::string LottieImporter::directory(const RemoteImporter& importer, const std::string& url){
    std::string name = safeFileName(urlBaseName(url));
    if (name.empty())
        name = "animation";
    return importer.importPath("lottie/" + urlHash(url, 12) + "-" + name);
}

ImportedResource LottieImporter::write(const std::string& url, const std::string& animationJson, const ImageSource& images,
                                       const ImportLocation& at){
    ImportedResource result;
    result.url = url;

    nlohmann::json animation = nlohmann::json::parse(animationJson, nullptr, false);
    if (!animation.is_object()){
        result.error = "the animation is not a JSON object";
        importer.report(codes::UnsupportedContent, at, url, "'" + url + "' is not a Lottie animation (the JSON does not parse)");
        return result;
    }

    const std::string dir = directory(importer, url);
    ImportStaging staging;
    std::set<std::string> usedNames;
    const auto assets = animation.find("assets");
    if (assets != animation.end() && assets->is_array()){
        for (nlohmann::json& asset : *assets){
            if (!asset.is_object() || asset.contains("layers") || !asset.contains("p") || !asset["p"].is_string())
                continue;
            const std::string p = asset["p"].get<std::string>();
            const bool embedded = (asset.contains("e") && asset["e"].is_number() && asset["e"].get<int>() == 1) ||
                                  p.compare(0, 5, "data:") == 0;
            if (embedded || p.empty())
                continue;
            const std::string u = asset.contains("u") && asset["u"].is_string() ? asset["u"].get<std::string>() : "";
            std::string data;
            if (!images(u, p, data))
                continue;   // reported by the source; the animation draws without it
            std::string name = assetFileName(p);
            if (name.empty())
                name = "image";
            std::string unique = name;
            for (int n = 2; usedNames.count(unique); n++)
                unique = std::to_string(n) + "-" + name;
            usedNames.insert(unique);
            staging.add(dir + "/images/" + unique, std::move(data));
            asset["u"] = "images/";
            asset["p"] = unique;
            asset["e"] = 0;
        }
    }
    const std::string jsonPath = dir + "/animation.json";
    staging.add(jsonPath, animation.dump());

    std::string host, path, query;
    splitUrl(url, host, path, query);
    ImportEntry entry;
    entry.url = url;
    entry.kind = "lottie";
    entry.path = jsonPath;
    for (const auto& pair : staging.files())
        entry.files.push_back(pair.first);
    // the JSON first, then its images
    std::stable_partition(entry.files.begin(), entry.files.end(), [&jsonPath](const std::string& file){ return file == jsonPath; });
    const bool lottieFiles = isLottieFilesHost(host);
    entry.source = lottieFiles ? "LottieFiles" : host;
    entry.licenseName = lottieFiles ? "Lottie Simple License (verify on lottiefiles.com)" : "unknown";
    if (!importer.finish(staging, {entry}, result))
        return result;
    if (lottieFiles)
        importer.report(codes::LicenseNote, at, url, "LottieFiles animation '" + url +
                        "': free animations use the Lottie Simple License; verify the license of this one on lottiefiles.com");
    else
        importer.report(codes::LicenseNote, at, url, "the license of the animation '" + url + "' is unknown: verify that the game may ship it");
    return result;
}

ImportedResource LottieImporter::runJson(const std::string& url, const std::string& finalUrl, const std::string& body, const ImportLocation& at){
    const std::string base = finalUrl.empty() ? url : finalUrl;
    return write(url, body, [this, &base, &at](const std::string& u, const std::string& p, std::string& data){
        const std::string imageUrl = resolveUrl(base, u + p);
        HttpResponse response;
        std::string error;
        return importer.fetch(imageUrl, at, response, data, error);
    }, at);
}

ImportedResource LottieImporter::runDotLottie(const std::string& url, const std::string& finalUrl, const std::string& body,
                                              const ImportLocation& at){
    (void)finalUrl;
    ImportedResource result;
    result.url = url;

    DotLottie archive;
    std::string error;
    if (!archive.open(reinterpret_cast<const uint8_t*>(body.data()), body.size(), error)){
        result.error = error;
        importer.report(codes::UnsupportedContent, at, url, "'" + url + "' is not a dotLottie archive: " + error);
        return result;
    }
    const std::string id = archive.getDefaultAnimationId();
    std::string json;
    if (!archive.readAnimation(id, json, error)){
        result.error = error;
        importer.report(codes::UnsupportedContent, at, url, "the dotLottie archive '" + url + "' has no readable animation '" + id + "': " + error);
        return result;
    }
    return write(url, json, [this, &archive, &url, &at](const std::string& u, const std::string& p, std::string& data){
        if (archive.readImage(u + p, data))
            return true;
        importer.report(codes::UnsupportedContent, at, url, "the dotLottie archive '" + url + "' lacks the image '" + u + p + "'");
        return false;
    }, at);
}

ImportedResource LottieImporter::runPage(const std::string& url, const std::string& finalUrl, const std::string& body, const ImportLocation& at){
    (void)finalUrl;
    ImportedResource result;
    result.url = url;

    const std::vector<std::string> candidates = findAnimationUrls(body);
    if (candidates.empty()){
        result.error = "no animation file found in the LottieFiles page";
        importer.report(codes::UnsupportedContent, at, url, "the LottieFiles page '" + url + "' links no .json or .lottie file",
                        "open the animation on lottiefiles.com and import its Lottie JSON or dotLottie URL");
        return result;
    }

    ImportedResource animation = importer.import(candidates.front(), "lottie", at);
    if (!animation.ok || !animation.entry()){
        result.error = animation.error;
        return result;
    }

    // the page URL resolves to the same files
    ImportEntry page = *animation.entry();
    page.url = url;
    page.fetched.clear();
    ImportStaging none;
    if (!importer.finish(none, {page}, result))
        return result;
    result.entries.insert(result.entries.end(), animation.entries.begin(), animation.entries.end());
    result.written = animation.written;
    return result;
}
