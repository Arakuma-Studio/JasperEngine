// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Lospec endpoint (https://lospec.com/palette-list/<slug>.json, {name, author, colors[]}) is
// the one Pixelorama uses (src/Autoload/Palettes.gd, MIT, Copyright (c) 2019-present Orama
// Interactive and contributors); it is undocumented, so every surprise is a typed error.

#include "Lospec.h"

#include "htmlui/HttpClient.h"

#include "json.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>

// file-local, in a namespace of their own: a unity build merges this file with the other pixel-art
// sources, whose helpers live in jasperengine::editor::pixelart
namespace jasperengine::editor::pixelart::lospecio {
namespace {

    constexpr const char* kLospecPrefix = "https://lospec.com/palette-list/";

    PaletteError lospecProblem(const std::string& kind, const std::string& message, const std::string& fix) {
        PaletteError error;
        error.kind = kind;
        error.message = message;
        error.fix = fix;
        return error;
    }

    PaletteError networkUnavailable(const std::string& detail) {
        std::string message = "Can't reach lospec.com (offline, or blocked by a proxy).";
        if (!detail.empty()) {
            std::string sentence = detail;
            sentence[0] = (char)std::toupper((unsigned char)sentence[0]);
            message += " " + sentence + (sentence.back() == '.' ? "" : ".");
        }
        return lospecProblem("network_unavailable", message, "Download the palette's .hex from its Lospec page and drop it on this window.");
    }

    PaletteError notFound(const std::string& slug) {
        return lospecProblem("lospec_not_found", "No Lospec palette called '" + slug + "'.", "Copy the slug from the palette's address on lospec.com.");
    }

    PaletteError badResponse(const std::string& slug, const std::string& why) {
        return lospecProblem("bad_response", "lospec.com sent an unexpected answer for '" + slug + "': " + why + ".",
                             "Download the palette's .hex from " + lospecPageUrl(slug) + " and drop it on this window.");
    }

    bool startsWith(std::string_view text, std::string_view prefix) {
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    bool endsWith(std::string_view text, std::string_view suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

}
}

using namespace jasperengine;

std::optional<std::string> editor::pixelart::normaliseSlug(std::string_view input) {
    std::string text;
    for (char c : input) text += (char)std::tolower((unsigned char)c);
    while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
    while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();

    std::string_view rest = text;
    if (lospecio::startsWith(rest, "lospec-palette://")) {
        rest.remove_prefix(17);
    } else if (rest.find("://") != std::string_view::npos || lospecio::startsWith(rest, "lospec.com/") ||
               lospecio::startsWith(rest, "www.lospec.com/")) {
        if (lospecio::startsWith(rest, "https://")) rest.remove_prefix(8);
        else if (lospecio::startsWith(rest, "http://")) rest.remove_prefix(7);
        else if (rest.find("://") != std::string_view::npos) return std::nullopt;
        if (lospecio::startsWith(rest, "www.")) rest.remove_prefix(4);
        if (!lospecio::startsWith(rest, "lospec.com/palette-list/")) return std::nullopt;
        rest.remove_prefix(24);
        const size_t query = rest.find_first_of("?#");
        if (query != std::string_view::npos) rest = rest.substr(0, query);
    }
    while (!rest.empty() && rest.back() == '/') rest.remove_suffix(1);
    if (lospecio::endsWith(rest, ".json")) rest.remove_suffix(5);

    if (rest.empty() || rest.size() > 64) return std::nullopt;
    for (char c : rest) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return std::nullopt;
    }
    return std::string(rest);
}

std::string editor::pixelart::lospecPageUrl(const std::string& slug) {
    return lospecio::kLospecPrefix + slug;
}

std::string editor::pixelart::lospecJsonUrl(const std::string& slug) {
    return lospecio::kLospecPrefix + slug + ".json";
}

std::variant<jasperengine::pixelart::Palette, editor::pixelart::PaletteError> editor::pixelart::parseLospecResponse(
    const std::string& slug, int status, const std::string& body, const std::string& todayIso) {
    namespace px = jasperengine::pixelart;
    using OJson = nlohmann::ordered_json;
    if (status == 404) return lospecio::notFound(slug);
    // no exceptions (the Web build has none): a malformed body is a discarded value
    const OJson root = OJson::parse(body, nullptr, false);
    if (root.is_discarded()) return lospecio::badResponse(slug, "not JSON");
    if (!root.is_object()) return lospecio::badResponse(slug, "not a palette object");
    // Lospec answers an unknown slug with {"error": "palette not found"}
    auto error = root.find("error");
    if (error != root.end() && root.find("colors") == root.end()) return lospecio::notFound(slug);
    if (status < 200 || status >= 300) return lospecio::badResponse(slug, "HTTP " + std::to_string(status));

    auto colors = root.find("colors");
    if (colors == root.end() || !colors->is_array() || colors->empty()) return lospecio::badResponse(slug, "no \"colors\" list");
    px::Palette palette;
    for (size_t i = 0; i < colors->size(); i++) {
        const OJson& entry = (*colors)[i];
        px::Rgba8 color;
        if (!entry.is_string() || !px::parseHex(entry.get<std::string>(), color)) {
            return lospecio::badResponse(slug, "colour " + std::to_string(i) + " is not a hex colour");
        }
        palette.colors.push_back(color);
    }
    if (palette.count() > px::Palette::kMaxColors) {
        PaletteError tooMany = lospecio::lospecProblem("too_many_colors", "Lospec's '" + slug + "' has " + std::to_string(palette.count()) +
                                                       " colours; a palette holds at most 256", "Reduce Colours… or Import the first 256");
        tooMany.field = "colors";
        return tooMany;
    }
    auto name = root.find("name");
    palette.name = name != root.end() && name->is_string() && !name->get<std::string>().empty() ? name->get<std::string>() : slug;
    auto author = root.find("author");
    if (author != root.end() && author->is_string()) palette.author = author->get<std::string>();
    palette.source.kind = "lospec";
    palette.source.id = slug;
    palette.source.url = lospecPageUrl(slug);
    palette.source.fetched = todayIso;

    std::vector<px::Diagnostic> diagnostics;
    if (!px::PaletteCodec::validate(palette, diagnostics)) {
        for (const px::Diagnostic& diagnostic : diagnostics) {
            if (diagnostic.severity == px::Diagnostic::Severity::Error) return lospecio::badResponse(slug, diagnostic.message);
        }
    }
    return palette;
}

std::variant<jasperengine::pixelart::Palette, editor::pixelart::PaletteError> editor::pixelart::fetchLospec(
    const std::string& slug, htmlui::HttpClient& client, const std::string& todayIso) {
    const std::string mirror = htmlui::HttpClient::mirrorDirectory();
    if (mirror.empty() && !htmlui::HttpClient::available()) return lospecio::networkUnavailable("curl is not installed");
    std::error_code ec;
    // the tests' offline control: a mirror that does not exist is no network at all
    if (!mirror.empty() && !std::filesystem::is_directory(std::filesystem::path(mirror), ec)) {
        return lospecio::networkUnavailable("the import mirror " + mirror + " does not exist");
    }
    client.setTimeoutSeconds(kLospecTimeoutSeconds);
    const htmlui::HttpResponse response = client.get(lospecJsonUrl(slug));
    if (response.status == 404) return lospecio::notFound(slug);
    if (response.status == 0) return lospecio::networkUnavailable(response.error);
    if (response.status >= 200 && response.status < 300 && !response.error.empty()) return lospecio::networkUnavailable(response.error);
    if (response.status < 200 || response.status >= 300) return lospecio::badResponse(slug, "HTTP " + std::to_string(response.status));
    std::string body;
    if (!htmlui::HttpClient::readBody(response, body)) return lospecio::badResponse(slug, "the answer could not be read");
    return parseLospecResponse(slug, response.status, body, todayIso);
}

std::string editor::pixelart::todayIsoDate() {
    // days since 1970-01-01 to a civil date (Howard Hinnant's days_from_civil inverse, public domain)
    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    long long days = seconds / 86400;
    if (seconds % 86400 < 0) days--;
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const long long dayOfEra = days - era * 146097;
    const long long yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const long long dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const long long monthIndex = (5 * dayOfYear + 2) / 153;
    const long long day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    const long long month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    const long long year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
    // (int: a year past 9999 is not a date this writes; month and day are 1-12 and 1-31)
    char text[32];
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", (int)year, (int)month, (int)day);
    return text;
}
